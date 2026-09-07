#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace janus {

namespace fs = std::filesystem;

inline constexpr std::size_t max_record_bytes = 8U * 1024U * 1024U;

enum class Harness { claude, codex };

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
};

struct Pair {
  fs::path claude;
  fs::path codex;
  std::uintmax_t claude_size = 0;
  std::uintmax_t codex_size = 0;
};

}  // namespace janus
