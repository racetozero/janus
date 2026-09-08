#pragma once

#include <set>
#include <vector>

#include "janus/jsonl.hpp"

namespace janus {

struct Options {
  fs::path claude_root = user_home() / ".claude/projects";
  fs::path codex_root = user_home() / ".codex/sessions";
  fs::path kiss_root = user_home() / ".kiss/agent/sessions";
  fs::path pi_root = user_home() / ".pi/agent/sessions";
  fs::path openclaw_root = user_home() / ".openclaw/agents";
  fs::path hermes_db = user_home() / ".hermes/state.db";
  fs::path opencode_db = user_home() / ".local/share/opencode/opencode.db";
  fs::path state = user_home() / ".local/state/janus/pairs.tsv";
  unsigned interval = 2;
  bool daemonize = false;
};

void daemonize_process();

class Syncer {
 public:
  Syncer(fs::path claude_root, fs::path codex_root, fs::path state);
  explicit Syncer(const Options& options);
  fs::path import_one(const fs::path& source, Harness source_harness);
  std::size_t sync();

 private:
  static std::string normal(const fs::path& path);
  static bool has_messages(const SessionRef& session);
  void load_pairs();
  void add_group(Group group);
  void save_pairs() const;

  std::vector<Store> stores_;
  fs::path state_;
  std::vector<Group> groups_;
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
