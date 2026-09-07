#include <catch2/catch_test_macros.hpp>
#include <fstream>

#include "janus/adapters.hpp"

namespace {

class TestDirectory {
 public:
  TestDirectory() : path_(janus::fs::temp_directory_path() / ("janus-test-" + janus::make_uuid())) {
    janus::fs::create_directories(path_);
  }
  ~TestDirectory() {
    std::error_code error;
    janus::fs::remove_all(path_, error);
  }
  const janus::fs::path& path() const { return path_; }

 private:
  janus::fs::path path_;
};

janus::fs::path write_claude_session(const janus::fs::path& root) {
  janus::fs::create_directories(root);
  const janus::fs::path path = root / "source.jsonl";
  std::ofstream output(path);
  output << "{\"type\":\"user\",\"uuid\":\"u1\",\"sessionId\":\"source\",\"cwd\":\"/tmp/work\","
            "\"timestamp\":\"2026-01-01T00:00:00Z\",\"message\":{\"role\":\"user\","
            "\"content\":\"hello\"}}\n"
         << "{\"type\":\"assistant\",\"uuid\":\"u2\",\"sessionId\":\"source\","
            "\"cwd\":\"/tmp/work\",\"timestamp\":\"2026-01-01T00:00:01Z\","
            "\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\","
            "\"text\":\"world\"}]}}\n";
  return path;
}

}  // namespace

TEST_CASE("JSON strings are escaped") { REQUIRE(janus::quote("a\n\"b\\") == "\"a\\n\\\"b\\\\\""); }

TEST_CASE("sessions convert in both directions") {
  TestDirectory temporary;
  const auto claude = write_claude_session(temporary.path() / "claude");
  const auto codex = janus::create_peer(temporary.path() / "codex", claude, janus::Harness::claude);
  std::size_t count = 0;
  const janus::Session session =
      janus::scan_session(codex, janus::Harness::codex, [&](const janus::Message&) { ++count; });
  REQUIRE(count == 2);
  REQUIRE(session.cwd == "/tmp/work");

  const auto round_trip =
      janus::create_peer(temporary.path() / "round-trip", codex, janus::Harness::codex);
  count = 0;
  janus::scan_session(round_trip, janus::Harness::claude, [&](const janus::Message&) { ++count; });
  REQUIRE(count == 2);
}

TEST_CASE("copy is idempotent") {
  TestDirectory temporary;
  const auto claude = write_claude_session(temporary.path() / "claude");
  const auto codex = janus::create_peer(temporary.path() / "codex", claude, janus::Harness::claude);
  REQUIRE(janus::copy_missing(claude, janus::Harness::claude, codex, janus::Harness::codex) == 0);
}
