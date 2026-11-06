#pragma once

#include <optional>
#include <set>
#include <vector>

#include "janus/adapters.hpp"
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

void daemonize_process(const fs::path& log);

class Syncer {
 public:
  Syncer(fs::path claude_root, fs::path codex_root, fs::path state);
  explicit Syncer(const Options& options);
  fs::path import_one(const fs::path& source, Harness source_harness);
  std::size_t sync();

 private:
  static std::string normal(const fs::path& path);
  static std::string session_key(const SessionRef& session);
  static bool has_messages(const SessionRef& session);
  void refresh_stores();
  void load_pairs();
  void add_group(Group group);
  std::size_t add_missing_peers(Group& group);
  std::size_t sync_group(Group& group);
  void record_group(const Group& group) const;
  void save_pairs() const;

  std::optional<Options> options_;
  std::vector<Store> stores_;
  fs::path state_;
  std::vector<Group> groups_;
  std::set<std::string> known_;
  Discovery discovery_;
  bool loaded_ = false;
};

class ProcessLock {
 public:
  explicit ProcessLock(const fs::path& state);
  ProcessLock(const ProcessLock&) = delete;
  ProcessLock& operator=(const ProcessLock&) = delete;
  ~ProcessLock();

 private:
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int descriptor_ = -1;
#endif
};

}  // namespace janus
