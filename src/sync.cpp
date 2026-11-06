#include "janus/sync.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <ostream>
#include <stdexcept>
#include <utility>

#include "janus/adapters.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace janus {

namespace {

constexpr const char* state_row_separators = "\t\r\n";

bool fits_state_row(const SessionRef& session) {
  return session.store.string().find_first_of(state_row_separators) == std::string::npos &&
         session.id.find_first_of(state_row_separators) == std::string::npos;
}

void write_group_rows(std::ostream& output, const Group& group) {
  for (const SessionRef& member : group.members) {
    output << "v2\t" << group.id << '\t' << harness_name(member.harness) << '\t'
           << member.store.string() << '\t' << member.id << '\t' << member.stamp << '\n';
  }
}

}  // namespace

void daemonize_process(const fs::path& log) {
#ifdef _WIN32
  static_cast<void>(log);
  throw std::runtime_error("--daemonize is not supported on Windows");
#else
  constexpr std::uintmax_t max_log_bytes = 1024U * 1024U;
  std::error_code log_error;
  const std::uintmax_t log_bytes = fs::file_size(log, log_error);
  if (!log_error && log_bytes > max_log_bytes) fs::rename(log, log.string() + ".1", log_error);
  // Open the log before the fork, so that a failure reaches the terminal.
  const int output = ::open(log.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0600);
  if (output < 0) {
    throw std::runtime_error("cannot open daemon log " + log.string() +
                             "; make the path writable or set a different --state path");
  }

  const pid_t first = ::fork();
  if (first < 0) throw std::runtime_error("cannot fork daemon");
  if (first > 0) ::_exit(0);
  if (::setsid() < 0) throw std::runtime_error("cannot create daemon session");

  const pid_t second = ::fork();
  if (second < 0) throw std::runtime_error("cannot fork daemon");
  if (second > 0) ::_exit(0);

  const int null = ::open("/dev/null", O_RDONLY);
  if (null < 0) throw std::runtime_error("cannot open /dev/null");
  if (::dup2(null, STDIN_FILENO) < 0 || ::dup2(output, STDOUT_FILENO) < 0 ||
      ::dup2(output, STDERR_FILENO) < 0) {
    throw std::runtime_error("cannot connect daemon streams");
  }
  if (null > STDERR_FILENO) ::close(null);
  if (output > STDERR_FILENO) ::close(output);
  std::cout << std::unitbuf;
#endif
}

Syncer::Syncer(fs::path claude_root, fs::path codex_root, fs::path state)
    : stores_{{Harness::claude, std::move(claude_root)}, {Harness::codex, std::move(codex_root)}},
      state_(std::move(state)) {}

Syncer::Syncer(const Options& options) : options_(options), state_(options.state) {
  refresh_stores();
}

void Syncer::refresh_stores() {
  if (!options_) return;
  const Options& options = *options_;
  std::vector<Store> stores;
  const auto add_store = [&](Harness harness, const fs::path& path) {
    if (path.string().find_first_of(state_row_separators) != std::string::npos) {
      throw std::runtime_error("the " + std::string(harness_name(harness)) + " store path " +
                               path.string() +
                               " contains a tab or a line break, which the state file cannot "
                               "store; set its Janus path option to a path without these");
    }
    stores.push_back({harness, path});
  };
  const auto add_directory = [&](Harness harness, const fs::path& path) {
    if (fs::is_directory(path)) add_store(harness, path);
  };
  const auto add_database = [&](Harness harness, const fs::path& path) {
    if (fs::is_regular_file(path)) add_store(harness, path);
  };
  add_directory(Harness::claude, options.claude_root);
  add_directory(Harness::codex, options.codex_root);
  add_directory(Harness::kiss, options.kiss_root);
  add_directory(Harness::pi, options.pi_root);
  if (fs::is_regular_file(options.openclaw_root)) {
    add_database(Harness::openclaw, options.openclaw_root);
  } else if (fs::is_directory(options.openclaw_root)) {
    const fs::path main_database = options.openclaw_root / "main/agent/openclaw-agent.sqlite";
    if (fs::is_regular_file(main_database)) {
      add_database(Harness::openclaw, main_database);
    } else {
      std::vector<fs::path> databases;
      std::error_code error;
      for (fs::recursive_directory_iterator iterator(
               options.openclaw_root, fs::directory_options::skip_permission_denied, error),
           end;
           iterator != end; iterator.increment(error)) {
        if (!error && iterator->is_regular_file(error) &&
            iterator->path().filename() == "openclaw-agent.sqlite") {
          databases.push_back(iterator->path());
        }
        error.clear();
      }
      if (!databases.empty()) {
        std::ranges::sort(databases);
        add_database(Harness::openclaw, databases.front());
      }
    }
  }
  add_database(Harness::hermes, options.hermes_db);
  add_database(Harness::opencode, options.opencode_db);
  stores_ = std::move(stores);
}

fs::path Syncer::import_one(const fs::path& source, Harness source_harness) {
  if (!fs::is_regular_file(source)) {
    throw std::runtime_error("session file does not exist: " + source.string());
  }
  load_pairs();
  const std::string source_name = normal(source);
  Group* group = nullptr;
  for (Group& candidate : groups_) {
    if (std::ranges::any_of(candidate.members, [&](const SessionRef& member) {
          return member.harness == source_harness && normal(member.store) == source_name;
        }))
      group = &candidate;
  }
  if (group == nullptr) {
    const Session metadata = scan_session(source, source_harness);
    SessionRef reference{source_harness, source, metadata.id,
                         static_cast<std::uint64_t>(fs::file_size(source))};
    if (!fits_state_row(reference)) {
      throw std::runtime_error("cannot import " + source.string() +
                               ": its path or session ID contains a tab or a line break, which "
                               "the state file cannot store; move the session to a path without "
                               "these, then run the import again");
    }
    add_group({make_uuid(), {std::move(reference)}});
    group = &groups_.back();
  } else {
    // A sync does not create a deleted session again, but an explicit import does.
    std::vector<SessionRef> present;
    for (const SessionRef& member : group->members) {
      if (session_status(member) == SessionStatus::missing) {
        known_.erase(session_key(member));
      } else {
        present.push_back(member);
      }
    }
    group->members = std::move(present);
  }
  add_missing_peers(*group);
  save_pairs();
  const auto peer = std::ranges::find_if(
      group->members, [&](const SessionRef& member) { return member.harness != source_harness; });
  if (peer != group->members.end()) return peer->store;
  if (std::ranges::none_of(stores_,
                           [&](const Store& store) { return store.harness != source_harness; }))
    throw std::runtime_error("no target harness store is available");
  throw std::runtime_error("cannot import " + source.string() +
                           " into any target store; fix the errors above, then run the import "
                           "again or run `janus sync`");
}

std::size_t Syncer::add_missing_peers(Group& group) {
  std::size_t created = 0;
  for (const Store& store : stores_) {
    if (std::ranges::any_of(group.members, [&](const SessionRef& member) {
          return member.harness == store.harness;
        }))
      continue;
    const auto source = std::ranges::find_if(group.members, [](const SessionRef& member) {
      return session_status(member) == SessionStatus::active;
    });
    if (source == group.members.end()) break;
    SessionRef peer;
    try {
      peer = create_peer(store, *source);
    } catch (const std::exception& error) {
      std::cerr << "cannot create " << harness_name(store.harness) << " session from "
                << source->store.string() << ": " << error.what()
                << "; fix the target store, and Janus tries again on the next sync\n";
      continue;
    }
    known_.insert(session_key(peer));
    group.members.push_back(std::move(peer));
    ++created;
    // Record the peer now. If the process stops before the final save, the next sync must not
    // read this peer as a new session.
    record_group(group);
  }
  return created;
}

std::size_t Syncer::sync_group(Group& group) {
  std::vector<SessionStatus> statuses;
  for (const SessionRef& member : group.members) statuses.push_back(session_status(member));
  if (std::ranges::all_of(statuses,
                          [](SessionStatus status) { return status == SessionStatus::missing; })) {
    for (const SessionRef& member : group.members) {
      known_.erase(session_key(member));
      // A new file at this path must count as new, even if it has the same size.
      discovery_.seen.erase(normal(member.store));
    }
    group.members.clear();
    return 0;
  }
  const std::size_t created = add_missing_peers(group);
  statuses.resize(group.members.size(), SessionStatus::active);
  for (std::size_t index = group.members.size() - created; index < group.members.size(); ++index) {
    std::cout << "created " << harness_name(group.members[index].harness) << " session "
              << group.members[index].id << '\n';
  }
  std::size_t changes = created;

  // Read the stamps before the copy and store these values after it. Then a message that arrives
  // during the copy changes the stamp, and the next sync finds that message.
  std::vector<std::size_t> active;
  std::vector<std::uint64_t> stamps(group.members.size(), unavailable_stamp);
  bool changed = false;
  for (std::size_t index = 0; index < group.members.size(); ++index) {
    // An unavailable stamp never matches, so the session catches up when it is active again.
    if (statuses[index] != SessionStatus::active) continue;
    stamps[index] = session_stamp(group.members[index]);
    if (stamps[index] != group.members[index].stamp) changed = true;
    active.push_back(index);
  }
  if (changed) {
    for (const std::size_t source : active) {
      for (const std::size_t target : active) {
        if (source != target) changes += copy_missing(group.members[source], group.members[target]);
      }
    }
  }
  for (std::size_t index = 0; index < group.members.size(); ++index) {
    group.members[index].stamp = stamps[index];
  }
  return changes;
}

std::size_t Syncer::sync() {
  load_pairs();
  refresh_stores();
  std::size_t failures = 0;
  for (const Store& store : stores_) {
    try {
      for (const SessionRef& source : discover_sessions(store, &discovery_)) {
        const std::string key = session_key(source);
        if (known_.contains(key)) continue;
        if (!fits_state_row(source)) {
          std::cerr << "skipped " << harness_name(source.harness) << " session "
                    << source.store.string()
                    << ": its path or ID contains a tab or a line break, which the state file "
                       "cannot store; move the session to a path without these to sync it\n";
          // This key is never saved. It stops the same message on each later sync.
          known_.insert(key);
          continue;
        }
        try {
          if (has_messages(source)) add_group({make_uuid(), {source}});
        } catch (const std::exception& error) {
          std::cerr << "cannot read " << harness_name(source.harness) << " session "
                    << source.store.string() << ": " << error.what()
                    << "; fix the session, and Janus tries again on the next sync\n";
          discovery_.seen.erase(normal(source.store));
          ++failures;
        }
      }
    } catch (const std::exception& error) {
      std::cerr << "cannot read " << harness_name(store.harness) << " store " << store.path.string()
                << ": " << error.what()
                << "; fix the store, and Janus tries again on the next sync\n";
      ++failures;
    }
  }
  failures += std::exchange(discovery_.failures, 0);
  std::size_t changes = 0;
  for (Group& group : groups_) {
    try {
      changes += sync_group(group);
    } catch (const std::exception& error) {
      std::cerr << "cannot sync group " << group.id << ": " << error.what()
                << "; other groups continue, and Janus tries this group again on the next sync\n";
      ++failures;
    }
  }
  std::erase_if(groups_, [](const Group& group) { return group.members.empty(); });
  save_pairs();
  if (failures > 0) {
    throw std::runtime_error("sync finished with errors (" + std::to_string(failures) +
                             "); all other sessions are synced; fix the errors above, and the "
                             "next sync tries again");
  }
  return changes;
}

std::string Syncer::session_key(const SessionRef& session) {
  return std::string(harness_name(session.harness)) + ':' + normal(session.store) + ':' +
         session.id;
}

std::string Syncer::normal(const fs::path& path) {
  return fs::absolute(path).lexically_normal().string();
}

bool Syncer::has_messages(const SessionRef& session) {
  bool found = false;
  scan_session(session, [&](const Message&) { found = true; });
  return found;
}

void Syncer::load_pairs() {
  if (loaded_) return;
  // Parse into local values. A failed parse must not leave half of the groups in this object.
  std::vector<Group> parsed;
  bool rewrite = false;
  if (fs::exists(state_)) {
    std::ifstream input(state_);
    if (!input) throw std::runtime_error("cannot read state file " + state_.string());
    std::size_t number = 0;
    for (std::string line; std::getline(input, line);) {
      ++number;
      if (line.empty()) continue;
      // A final row with no newline is the sign of a process that stopped during an append.
      const bool unterminated = input.eof();
      rewrite = rewrite || unterminated;
      std::vector<std::string> fields;
      for (std::size_t begin = 0;;) {
        const std::size_t end = line.find('\t', begin);
        fields.push_back(line.substr(begin, end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
      }
      try {
        if (fields.size() == 4) {
          const fs::path claude = fields[0];
          const fs::path codex = fields[1];
          if (!fs::exists(claude) || !fs::exists(codex)) continue;
          const Session claude_session = scan_session(claude, Harness::claude);
          const Session codex_session = scan_session(codex, Harness::codex);
          parsed.push_back({make_uuid(),
                            {{Harness::claude, claude, claude_session.id, std::stoull(fields[2])},
                             {Harness::codex, codex, codex_session.id, std::stoull(fields[3])}}});
        } else if (fields.size() == 6 && fields[0] == "v2") {
          Group* group = nullptr;
          for (Group& candidate : parsed)
            if (candidate.id == fields[1]) group = &candidate;
          if (group == nullptr) {
            parsed.push_back({fields[1], {}});
            group = &parsed.back();
          }
          SessionRef member{parse_harness(fields[2]), fields[3], fields[4], std::stoull(fields[5])};
          // A group can repeat a row, because each new peer appends all rows of its group.
          if (std::ranges::none_of(group->members, [&](const SessionRef& other) {
                return other.harness == member.harness && other.store == member.store &&
                       other.id == member.id;
              }))
            group->members.push_back(std::move(member));
        } else {
          throw std::runtime_error("the row has an unknown form");
        }
      } catch (const std::exception& error) {
        if (unterminated) break;
        throw std::runtime_error(
            "state file " + state_.string() + " line " + std::to_string(number) +
            " is invalid: " + error.what() +
            "; each row must be `v2, group ID, harness, path, session ID, stamp` or the legacy "
            "`Claude path, Codex path, Claude stamp, Codex stamp`, with tabs between values; "
            "repair or delete this row, then run Janus again");
      }
    }
  }
  groups_.clear();
  known_.clear();
  for (Group& group : parsed) add_group(std::move(group));
  // Remove the unterminated row now, because a later append must start on a new line.
  if (rewrite) save_pairs();
  loaded_ = true;
}

void Syncer::add_group(Group group) {
  for (const SessionRef& member : group.members) {
    if (known_.contains(session_key(member))) return;
  }
  for (const SessionRef& member : group.members) known_.insert(session_key(member));
  groups_.push_back(std::move(group));
}

void Syncer::record_group(const Group& group) const {
  fs::create_directories(state_.parent_path());
  std::ofstream output(state_, std::ios::app);
  write_group_rows(output, group);
  output.flush();
  if (!output) {
    throw std::runtime_error("cannot write state file " + state_.string() +
                             "; make the path writable or set a different --state path");
  }
}

void Syncer::save_pairs() const {
  fs::create_directories(state_.parent_path());
  const fs::path temporary = state_.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    for (const Group& group : groups_) write_group_rows(output, group);
    output.flush();
    // A failed write must not replace the good state file.
    if (!output) {
      throw std::runtime_error("cannot write " + temporary.string() +
                               "; make the path writable or set a different --state path");
    }
  }
  fs::rename(temporary, state_);
}

ProcessLock::ProcessLock(const fs::path& state) {
  fs::create_directories(state.parent_path());
  const fs::path path = state.string() + ".lock";
#ifdef _WIN32
  const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("another Janus process uses this state file");
  }
  handle_ = handle;
#else
  descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
  if (descriptor_ < 0 || ::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
    if (descriptor_ >= 0) ::close(descriptor_);
    throw std::runtime_error("another Janus process uses this state file");
  }
#endif
}

ProcessLock::~ProcessLock() {
#ifdef _WIN32
  if (handle_ != nullptr) CloseHandle(static_cast<HANDLE>(handle_));
#else
  if (descriptor_ >= 0) ::close(descriptor_);
#endif
}

}  // namespace janus
