#include "janus/benchmark.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

#include "janus/adapters.hpp"

namespace janus {
namespace {

class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(std::string_view label)
      : path_(fs::temp_directory_path() / (std::string(label) + '-' + make_uuid())) {
    fs::create_directories(path_);
  }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

}  // namespace

void self_test() {
  TemporaryDirectory temporary("janus-test");
  const fs::path base = temporary.path();
  fs::create_directories(base / "claude/project");
  fs::create_directories(base / "codex");
  const fs::path claude = base / "claude/project/source.jsonl";
  {
    std::ofstream output(claude);
    output << "{\"type\":\"user\",\"uuid\":\"u1\",\"parentUuid\":null,\"sessionId\":\"source\","
              "\"cwd\":\"/tmp/work\",\"timestamp\":\"2026-01-01T00:00:00Z\","
              "\"message\":{\"role\":\"user\",\"content\":\"hello\"}}\n"
           << "{\"type\":\"assistant\",\"uuid\":\"u2\",\"parentUuid\":\"u1\","
              "\"sessionId\":\"source\",\"cwd\":\"/tmp/work\","
              "\"timestamp\":\"2026-01-01T00:00:01Z\",\"message\":{\"role\":\"assistant\","
              "\"content\":[{\"type\":\"text\",\"text\":\"world\"}]}}\n";
  }
  const fs::path codex = create_peer(base / "codex", claude, Harness::claude);
  std::size_t codex_messages = 0;
  const Session parsed =
      scan_session(codex, Harness::codex, [&](const Message&) { ++codex_messages; });
  if (codex_messages != 2 || parsed.cwd != "/tmp/work") {
    throw std::runtime_error("Codex adapter test failed");
  }
  const fs::path round_trip = create_peer(base / "claude", codex, Harness::codex);
  std::size_t claude_messages = 0;
  scan_session(round_trip, Harness::claude, [&](const Message&) { ++claude_messages; });
  if (claude_messages != 2) throw std::runtime_error("Claude adapter test failed");
  if (copy_missing(claude, Harness::claude, codex, Harness::codex) != 0) {
    throw std::runtime_error("deduplication test failed");
  }
  std::cout << "self-test passed\n";
}

void benchmark(std::size_t message_count, bool header) {
  TemporaryDirectory temporary("janus-benchmark");
  const fs::path claude_root = temporary.path() / "claude";
  const fs::path codex_root = temporary.path() / "codex";
  fs::create_directories(claude_root / "project");
  fs::create_directories(codex_root);
  const fs::path claude = claude_root / "project/source.jsonl";
  Session source{"benchmark", "/tmp/janus-benchmark", {}, 0};
  {
    Appender output(claude);
    const std::string payload(240, 'x');
    for (std::size_t index = 0; index < message_count; ++index) {
      append_claude(output, source,
                    {index % 2 == 0 ? "user" : "assistant", payload + std::to_string(index),
                     "2026-01-01T00:00:00Z"});
    }
  }

  const auto forward_start = std::chrono::steady_clock::now();
  const fs::path codex = create_peer(codex_root, claude, Harness::claude);
  const auto forward_end = std::chrono::steady_clock::now();
  const auto reverse_start = std::chrono::steady_clock::now();
  create_peer(claude_root, codex, Harness::codex);
  const auto reverse_end = std::chrono::steady_clock::now();

  const double mebibytes = static_cast<double>(fs::file_size(claude)) / (1024.0 * 1024.0);
  const double forward_ms =
      std::chrono::duration<double, std::milli>(forward_end - forward_start).count();
  const double reverse_ms =
      std::chrono::duration<double, std::milli>(reverse_end - reverse_start).count();
  if (header) {
    std::cout << "messages\tinput_MiB\tclaude_to_codex_ms\tMiB_per_s\tcodex_to_claude_ms\n";
  }
  std::cout << message_count << '\t' << std::fixed << std::setprecision(2) << mebibytes << '\t'
            << forward_ms << '\t' << mebibytes * 1000.0 / forward_ms << '\t' << reverse_ms << '\n';
}

}  // namespace janus
