#pragma once

#include <set>
#include <vector>

#include "janus/jsonl.hpp"

namespace janus {

struct Options {
  fs::path claude_root = user_home() / ".claude/projects";
  fs::path codex_root = user_home() / ".codex/sessions";
  fs::path state = user_home() / ".local/state/janus/pairs.tsv";
  unsigned interval = 2;
  bool daemonize = false;
};

void daemonize_process();

class Syncer {
 public:
  Syncer(fs::path claude_root, fs::path codex_root, fs::path state);
  fs::path import_one(const fs::path& source, Harness source_harness);
  std::size_t sync();

 private:
  static std::string normal(const fs::path& path);
  static bool has_messages(const fs::path& path, Harness harness);
  static std::vector<fs::path> discover(const fs::path& root, Harness harness);
  void load_pairs();
  void add_pair(const fs::path& claude, const fs::path& codex,
                std::uintmax_t claude_size = UINTMAX_MAX, std::uintmax_t codex_size = UINTMAX_MAX);
  void save_pairs() const;

  fs::path claude_root_;
  fs::path codex_root_;
  fs::path state_;
  std::vector<Pair> pairs_;
  std::set<std::string> known_;
};

class ProcessLock {
 public:
  explicit ProcessLock(const fs::path& state);
  ProcessLock(const ProcessLock&) = delete;
  ProcessLock& operator=(const ProcessLock&) = delete;
  ~ProcessLock();

 private:
#ifndef _WIN32
  int descriptor_ = -1;
#endif
};

}  // namespace janus
