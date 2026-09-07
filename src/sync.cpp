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
    : claude_root_(std::move(claude_root)),
      codex_root_(std::move(codex_root)),
      state_(std::move(state)) {}

fs::path Syncer::import_one(const fs::path& source, Harness source_harness) {
  if (!fs::is_regular_file(source)) {
    throw std::runtime_error("session file does not exist: " + source.string());
  }
  load_pairs();
  const std::string source_name = normal(source);
  for (const Pair& pair : pairs_) {
    if (source_harness == Harness::claude && normal(pair.claude) == source_name) return pair.codex;
    if (source_harness == Harness::codex && normal(pair.codex) == source_name) return pair.claude;
  }
  const fs::path target = create_peer(
      source_harness == Harness::claude ? codex_root_ : claude_root_, source, source_harness);
  if (source_harness == Harness::claude)
    add_pair(source, target);
  else
    add_pair(target, source);
  save_pairs();
  return target;
}

std::size_t Syncer::sync() {
  load_pairs();
  std::size_t changes = 0;
  for (const fs::path& path : discover(claude_root_, Harness::claude)) {
    if (known_.contains(normal(path)) || !has_messages(path, Harness::claude)) continue;
    const fs::path target = create_peer(codex_root_, path, Harness::claude);
    add_pair(path, target);
    ++changes;
    std::cout << "created Codex session " << target.string() << '\n';
  }
  for (const fs::path& path : discover(codex_root_, Harness::codex)) {
    if (known_.contains(normal(path)) || !has_messages(path, Harness::codex)) continue;
    const fs::path target = create_peer(claude_root_, path, Harness::codex);
    add_pair(target, path);
    ++changes;
    std::cout << "created Claude session " << target.string() << '\n';
  }
  for (Pair& pair : pairs_) {
    if (!fs::exists(pair.claude) || !fs::exists(pair.codex)) continue;
    const std::uintmax_t claude_size = fs::file_size(pair.claude);
    const std::uintmax_t codex_size = fs::file_size(pair.codex);
    if (claude_size != pair.claude_size) {
      changes += copy_missing(pair.claude, Harness::claude, pair.codex, Harness::codex);
    }
    if (codex_size != pair.codex_size) {
      changes += copy_missing(pair.codex, Harness::codex, pair.claude, Harness::claude);
    }
    pair.claude_size = fs::file_size(pair.claude);
    pair.codex_size = fs::file_size(pair.codex);
  }
  save_pairs();
  return changes;
}

std::string Syncer::normal(const fs::path& path) {
  return fs::absolute(path).lexically_normal().string();
}

bool Syncer::has_messages(const fs::path& path, Harness harness) {
  bool found = false;
  scan_session(path, harness, [&](const Message&) { found = true; });
  return found;
}

std::vector<fs::path> Syncer::discover(const fs::path& root, Harness harness) {
  std::vector<fs::path> paths;
  if (!fs::exists(root)) return paths;
  std::error_code error;
  for (fs::recursive_directory_iterator
           iterator(root, fs::directory_options::skip_permission_denied, error),
       end;
       iterator != end; iterator.increment(error)) {
    if (error) {
      error.clear();
      continue;
    }
    if (!iterator->is_regular_file(error) || iterator->path().extension() != ".jsonl") continue;
    const std::string name = iterator->path().filename().string();
    if (harness == Harness::codex ? name.starts_with("rollout-")
                                  : name != "skill-injections.jsonl") {
      paths.push_back(iterator->path());
    }
  }
  std::ranges::sort(paths);
  return paths;
}

void Syncer::load_pairs() {
  if (!pairs_.empty() || !fs::exists(state_)) return;
  std::ifstream input(state_);
  for (std::string line; std::getline(input, line);) {
    const std::size_t first = line.find('\t');
    if (first == std::string::npos) continue;
    const std::size_t second = line.find('\t', first + 1);
    const std::size_t third = second == std::string::npos ? second : line.find('\t', second + 1);
    const fs::path claude = line.substr(0, first);
    const fs::path codex = line.substr(first + 1, second - first - 1);
    const std::uintmax_t claude_size =
        second == std::string::npos ? 0 : std::stoull(line.substr(second + 1, third - second - 1));
    const std::uintmax_t codex_size =
        third == std::string::npos ? 0 : std::stoull(line.substr(third + 1));
    if (fs::exists(claude) && fs::exists(codex)) {
      add_pair(claude, codex, claude_size, codex_size);
    }
  }
}

void Syncer::add_pair(const fs::path& claude, const fs::path& codex, std::uintmax_t claude_size,
                      std::uintmax_t codex_size) {
  const std::string left = normal(claude);
  const std::string right = normal(codex);
  if (known_.contains(left) || known_.contains(right)) return;
  pairs_.push_back({left, right, claude_size == UINTMAX_MAX ? fs::file_size(claude) : claude_size,
                    codex_size == UINTMAX_MAX ? fs::file_size(codex) : codex_size});
  known_.insert(left);
  known_.insert(right);
}

void Syncer::save_pairs() const {
  fs::create_directories(state_.parent_path());
  const fs::path temporary = state_.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write " + temporary.string());
    for (const Pair& pair : pairs_) {
      output << pair.claude.string() << '\t' << pair.codex.string() << '\t' << pair.claude_size
             << '\t' << pair.codex_size << '\n';
    }
  }
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
