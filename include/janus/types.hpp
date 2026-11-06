#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace janus {

namespace fs = std::filesystem;

inline constexpr std::size_t max_record_bytes = 8U * 1024U * 1024U;
inline constexpr std::uint64_t unavailable_stamp = UINT64_MAX;

enum class Harness { claude, codex, kiss, pi, openclaw, hermes, opencode };

// Janus reads and writes active sessions only. A group ends when all its members are missing.
enum class SessionStatus { active, inactive, missing };

std::string_view harness_name(Harness harness);
Harness parse_harness(std::string_view name);

struct Message {
  std::string role;
  std::string text;
  std::string timestamp;
};

struct Session {
  std::string id;
  std::string cwd;
  std::string last_uuid;
  std::uint64_t last_ordinal = 0;
  std::string title;
  std::string first_user;
};

struct SessionRef {
  Harness harness = Harness::claude;
  fs::path store;
  std::string id;
  std::uint64_t stamp = 0;
};

struct Store {
  Harness harness = Harness::claude;
  fs::path path;
};

struct Group {
  std::string id;
  std::vector<SessionRef> members;
};

}  // namespace janus
