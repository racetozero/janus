#include "janus/sync.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "janus/adapters.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace janus {

void daemonize_process() {
#ifdef _WIN32
  throw std::runtime_error("--daemonize is not supported on Windows");
#else
  const pid_t first = ::fork();
  if (first < 0) throw std::runtime_error("cannot fork daemon");
  if (first > 0) ::_exit(0);
  if (::setsid() < 0) throw std::runtime_error("cannot create daemon session");

  const pid_t second = ::fork();
  if (second < 0) throw std::runtime_error("cannot fork daemon");
  if (second > 0) ::_exit(0);

  const int null = ::open("/dev/null", O_RDWR);
  if (null < 0) throw std::runtime_error("cannot open /dev/null");
  for (const int descriptor : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
    if (::dup2(null, descriptor) < 0) {
      ::close(null);
      throw std::runtime_error("cannot close daemon streams");
    }
  }
  if (null > STDERR_FILENO) ::close(null);
#endif
}

Syncer::Syncer(fs::path claude_root, fs::path codex_root, fs::path state)
    : stores_{{Harness::claude, std::move(claude_root)}, {Harness::codex, std::move(codex_root)}},
      state_(std::move(state)) {}

Syncer::Syncer(const Options& options) : state_(options.state) {
  const auto add_directory = [&](Harness harness, const fs::path& path) {
    if (fs::is_directory(path)) stores_.push_back({harness, path});
  };
  const auto add_database = [&](Harness harness, const fs::path& path) {
    if (fs::is_regular_file(path)) stores_.push_back({harness, path});
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
}

fs::path Syncer::import_one(const fs::path& source, Harness source_harness) {
  if (!fs::is_regular_file(source)) {
    throw std::runtime_error("session file does not exist: " + source.string());
  }
  load_pairs();
  const Session metadata = scan_session(source, source_harness);
  SessionRef source_reference{source_harness, source, metadata.id,
                              static_cast<std::uint64_t>(fs::file_size(source))};
  const std::string source_name = normal(source);
  for (const Group& group : groups_) {
    for (const SessionRef& member : group.members) {
      if (member.harness == source_harness && normal(member.store) == source_name) {
        for (const SessionRef& peer : group.members)
          if (peer.harness != source_harness) return peer.store;
      }
    }
  }
  Group group{make_uuid(), {source_reference}};
  fs::path result;
  for (const Store& store : stores_) {
    if (store.harness == source_harness) continue;
    SessionRef peer = create_peer(store, source_reference);
    if (result.empty()) result = peer.store;
    group.members.push_back(std::move(peer));
  }
  if (result.empty()) throw std::runtime_error("no target harness store is available");
  add_group(std::move(group));
  save_pairs();
  return result;
}

std::size_t Syncer::sync() {
  load_pairs();
  std::size_t changes = 0;
  for (const Store& store : stores_) {
    for (SessionRef source : discover_sessions(store)) {
      const std::string key =
          std::string(harness_name(source.harness)) + ':' + normal(source.store) + ':' + source.id;
      if (known_.contains(key) || !has_messages(source)) continue;
      Group group{make_uuid(), {source}};
      for (const Store& target : stores_) {
        if (target.harness == source.harness) continue;
        SessionRef peer = create_peer(target, source);
        std::cout << "created " << harness_name(target.harness) << " session " << peer.id << '\n';
        group.members.push_back(std::move(peer));
        ++changes;
      }
      add_group(std::move(group));
    }
  }
  for (Group& group : groups_) {
    for (const Store& store : stores_) {
      if (std::ranges::any_of(group.members, [&](const SessionRef& member) {
            return member.harness == store.harness;
          }))
        continue;
      const auto source = std::ranges::find_if(
          group.members, [](const SessionRef& member) { return fs::exists(member.store); });
      if (source == group.members.end()) break;
      SessionRef peer = create_peer(store, *source);
      known_.insert(std::string(harness_name(peer.harness)) + ':' + normal(peer.store) + ':' +
                    peer.id);
      std::cout << "created " << harness_name(store.harness) << " session " << peer.id << '\n';
      group.members.push_back(std::move(peer));
      ++changes;
    }
    bool changed = false;
    for (SessionRef& member : group.members) {
      if (!fs::exists(member.store)) continue;
      if (session_stamp(member) != member.stamp) changed = true;
    }
    if (!changed) continue;
    for (const SessionRef& source : group.members) {
      if (!fs::exists(source.store)) continue;
      for (const SessionRef& target : group.members) {
        if (&source != &target && fs::exists(target.store)) changes += copy_missing(source, target);
      }
    }
    for (SessionRef& member : group.members) member.stamp = session_stamp(member);
  }
  save_pairs();
  return changes;
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
  if (!groups_.empty() || !fs::exists(state_)) return;
  std::ifstream input(state_);
  for (std::string line; std::getline(input, line);) {
    std::vector<std::string> fields;
    for (std::size_t begin = 0;;) {
      const std::size_t end = line.find('\t', begin);
      fields.push_back(line.substr(begin, end - begin));
      if (end == std::string::npos) break;
      begin = end + 1;
    }
    if (fields.size() == 4) {
      const fs::path claude = fields[0];
      const fs::path codex = fields[1];
      if (!fs::exists(claude) || !fs::exists(codex)) continue;
      const Session claude_session = scan_session(claude, Harness::claude);
      const Session codex_session = scan_session(codex, Harness::codex);
      groups_.push_back({make_uuid(),
                         {{Harness::claude, claude, claude_session.id, std::stoull(fields[2])},
                          {Harness::codex, codex, codex_session.id, std::stoull(fields[3])}}});
    } else if (fields.size() == 6 && fields[0] == "v2") {
      Group* group = nullptr;
      for (Group& candidate : groups_)
        if (candidate.id == fields[1]) group = &candidate;
      if (group == nullptr) {
        groups_.push_back({fields[1], {}});
        group = &groups_.back();
      }
      SessionRef member{parse_harness(fields[2]), fields[3], fields[4], std::stoull(fields[5])};
      if (fs::exists(member.store)) group->members.push_back(std::move(member));
    }
  }
  std::vector<Group> loaded = std::move(groups_);
  groups_.clear();
  known_.clear();
  for (Group& group : loaded) add_group(std::move(group));
}

void Syncer::add_group(Group group) {
  for (const SessionRef& member : group.members) {
    const std::string key =
        std::string(harness_name(member.harness)) + ':' + normal(member.store) + ':' + member.id;
    if (known_.contains(key)) return;
  }
  for (const SessionRef& member : group.members) {
    known_.insert(std::string(harness_name(member.harness)) + ':' + normal(member.store) + ':' +
                  member.id);
  }
  groups_.push_back(std::move(group));
}

void Syncer::save_pairs() const {
  fs::create_directories(state_.parent_path());
  const fs::path temporary = state_.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write " + temporary.string());
    for (const Group& group : groups_) {
      for (const SessionRef& member : group.members) {
        output << "v2\t" << group.id << '\t' << harness_name(member.harness) << '\t'
               << member.store.string() << '\t' << member.id << '\t' << member.stamp << '\n';
      }
    }
  }
#ifdef _WIN32
  fs::remove(state_);
#endif
  fs::rename(temporary, state_);
}

ProcessLock::ProcessLock(const fs::path& state) {
  fs::create_directories(state.parent_path());
#ifndef _WIN32
  const fs::path path = state.string() + ".lock";
  descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
  if (descriptor_ < 0 || ::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
    if (descriptor_ >= 0) ::close(descriptor_);
    throw std::runtime_error("another Janus process uses this state file");
  }
#endif
}

ProcessLock::~ProcessLock() {
#ifndef _WIN32
  if (descriptor_ >= 0) ::close(descriptor_);
#endif
}

}  // namespace janus
