#include "janus/benchmark.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>

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

void benchmark(std::size_t message_count, bool header) {
  TemporaryDirectory temporary("janus-benchmark");
  const fs::path claude_root = temporary.path() / "claude";
  const fs::path codex_root = temporary.path() / "codex";
  fs::create_directories(claude_root / "project");
  fs::create_directories(codex_root);
  const fs::path claude = claude_root / "project/source.jsonl";
  Session source{"benchmark", "/tmp/janus-benchmark", {}, 0, {}, {}};
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
