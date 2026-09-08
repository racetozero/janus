#include <sqlite3.h>

#include <catch2/catch_test_macros.hpp>
#include <fstream>

#include "janus/adapters.hpp"
#include "janus/sync.hpp"

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

void sql(const janus::fs::path& path, const char* statement) {
  sqlite3* database = nullptr;
  REQUIRE(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK);
  char* error = nullptr;
  const int result = sqlite3_exec(database, statement, nullptr, nullptr, &error);
  INFO(std::string(error == nullptr ? "" : error));
  sqlite3_free(error);
  sqlite3_close(database);
  REQUIRE(result == SQLITE_OK);
}

janus::SessionRef source_ref(const janus::fs::path& path) {
  return {janus::Harness::claude, path, "source", janus::fs::file_size(path)};
}

}  // namespace

TEST_CASE("JSON strings are escaped") { REQUIRE(janus::quote("a\n\"b\\") == "\"a\\n\\\"b\\\\\""); }

TEST_CASE("imported titles identify the original harness") {
  janus::Session source;
  source.id = "session";
  source.title = "Fix login";
  for (const janus::Harness harness :
       {janus::Harness::claude, janus::Harness::codex, janus::Harness::kiss, janus::Harness::pi,
        janus::Harness::openclaw, janus::Harness::hermes, janus::Harness::opencode}) {
    REQUIRE(janus::imported_title(harness, source) ==
            '[' + std::string(janus::harness_name(harness)) + "]: Fix login");
  }
  source.title = "[pi]: Fix login";
  REQUIRE(janus::imported_title(janus::Harness::claude, source) == source.title);
}

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

TEST_CASE("KISS and Pi peers use native records and origin names") {
  TestDirectory temporary;
  const auto source = write_claude_session(temporary.path() / "claude");
  for (const janus::Harness harness : {janus::Harness::kiss, janus::Harness::pi}) {
    const auto peer = janus::create_peer({harness, temporary.path() / janus::harness_name(harness)},
                                         source_ref(source));
    std::size_t count = 0;
    const janus::Session session =
        janus::scan_session(peer, [&](const janus::Message&) { ++count; });
    REQUIRE(count == 2);
    REQUIRE(session.title == "[claude]: hello");
    REQUIRE(janus::copy_missing(source_ref(source), peer) == 0);
  }
}

TEST_CASE("Codex title comes from the session index") {
  TestDirectory temporary;
  const auto source = write_claude_session(temporary.path() / "claude");
  const auto peer = janus::create_peer({janus::Harness::codex, temporary.path() / "codex/sessions"},
                                       source_ref(source));
  REQUIRE(janus::scan_session(peer).title == "[claude]: hello");
}

TEST_CASE("Hermes peer uses the session database") {
  TestDirectory temporary;
  const auto source = write_claude_session(temporary.path() / "claude");
  const auto database = temporary.path() / "state.db";
  sql(database,
      "CREATE TABLE sessions(id TEXT PRIMARY KEY, source TEXT NOT NULL, started_at REAL NOT NULL, "
      "message_count INTEGER DEFAULT 0, tool_call_count INTEGER DEFAULT 0, cwd TEXT, title TEXT, "
      "title_source TEXT, last_activity_at REAL, archived INTEGER DEFAULT 0, hidden INTEGER "
      "DEFAULT 0);"
      "CREATE TABLE messages(id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL, role "
      "TEXT "
      "NOT NULL, content TEXT, timestamp REAL NOT NULL, active INTEGER DEFAULT 1);");
  const auto peer = janus::create_peer({janus::Harness::hermes, database}, source_ref(source));
  std::size_t count = 0;
  const auto session = janus::scan_session(peer, [&](const janus::Message&) { ++count; });
  REQUIRE(count == 2);
  REQUIRE(session.title == "[claude]: hello");
  REQUIRE(janus::copy_missing(source_ref(source), peer) == 0);
}

TEST_CASE("OpenCode peer uses session message and part rows") {
  TestDirectory temporary;
  const auto source = write_claude_session(temporary.path() / "claude");
  const auto database = temporary.path() / "opencode.db";
  sql(database,
      "CREATE TABLE project(id TEXT PRIMARY KEY, worktree TEXT NOT NULL, time_updated INTEGER NOT "
      "NULL);"
      "INSERT INTO project VALUES('global','/',1);"
      "CREATE TABLE session(id TEXT PRIMARY KEY, project_id TEXT NOT NULL, parent_id TEXT, slug "
      "TEXT "
      "NOT NULL, directory TEXT NOT NULL, title TEXT NOT NULL, version TEXT NOT NULL, time_created "
      "INTEGER NOT NULL, time_updated INTEGER NOT NULL, time_archived INTEGER);"
      "CREATE TABLE message(id TEXT PRIMARY KEY, session_id TEXT NOT NULL, time_created INTEGER "
      "NOT "
      "NULL, time_updated INTEGER NOT NULL, data TEXT NOT NULL);"
      "CREATE TABLE part(id TEXT PRIMARY KEY, message_id TEXT NOT NULL, session_id TEXT NOT NULL, "
      "time_created INTEGER NOT NULL, time_updated INTEGER NOT NULL, data TEXT NOT NULL);");
  const auto peer = janus::create_peer({janus::Harness::opencode, database}, source_ref(source));
  std::size_t count = 0;
  const auto session = janus::scan_session(peer, [&](const janus::Message&) { ++count; });
  REQUIRE(count == 2);
  REQUIRE(session.title == "[claude]: hello");
  REQUIRE(janus::copy_missing(source_ref(source), peer) == 0);
}

TEST_CASE("OpenClaw peer uses transcript event rows") {
  TestDirectory temporary;
  const auto source = write_claude_session(temporary.path() / "claude");
  const auto database = temporary.path() / "openclaw-agent.sqlite";
  sql(database,
      "CREATE TABLE session_nodes(session_key TEXT PRIMARY KEY, current_session_id TEXT NOT NULL, "
      "entry_json TEXT NOT NULL, entry_valid INTEGER NOT NULL, updated_at INTEGER NOT NULL, "
      "created_at "
      "INTEGER, created_via TEXT, created_actor_type TEXT, display_name TEXT, label TEXT);"
      "CREATE TABLE session_windows(session_id TEXT PRIMARY KEY, session_key TEXT NOT NULL, "
      "session_scope "
      "TEXT NOT NULL, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, started_at "
      "INTEGER, "
      "ended_at INTEGER, transcript_updated_at INTEGER, display_name TEXT);"
      "CREATE TABLE transcript_events(session_id TEXT NOT NULL, seq INTEGER NOT NULL, event_json "
      "TEXT NOT "
      "NULL, created_at INTEGER NOT NULL, PRIMARY KEY(session_id, seq));"
      "CREATE TABLE transcript_rewrite_watermarks(session_id TEXT PRIMARY KEY, generation TEXT NOT "
      "NULL, "
      "updated_at INTEGER NOT NULL);");
  const auto peer = janus::create_peer({janus::Harness::openclaw, database}, source_ref(source));
  std::size_t count = 0;
  const auto session = janus::scan_session(peer, [&](const janus::Message&) { ++count; });
  REQUIRE(count == 2);
  REQUIRE(session.title == "[claude]: hello");
  REQUIRE(janus::copy_missing(source_ref(source), peer) == 0);
}

TEST_CASE("sync creates one idempotent group across file harnesses") {
  TestDirectory temporary;
  janus::Options options;
  options.claude_root = temporary.path() / "claude";
  options.codex_root = temporary.path() / "codex";
  options.kiss_root = temporary.path() / "kiss";
  options.pi_root = temporary.path() / "pi";
  options.openclaw_root = temporary.path() / "no-openclaw";
  options.hermes_db = temporary.path() / "no-hermes.db";
  options.opencode_db = temporary.path() / "no-opencode.db";
  options.state = temporary.path() / "state/pairs.tsv";
  write_claude_session(options.claude_root);
  janus::fs::create_directories(options.codex_root);
  janus::fs::create_directories(options.kiss_root);
  janus::fs::create_directories(options.pi_root);

  janus::Syncer syncer(options);
  REQUIRE(syncer.sync() == 3);
  REQUIRE(syncer.sync() == 0);

  const auto pi_sessions = janus::discover_sessions({janus::Harness::pi, options.pi_root});
  REQUIRE(pi_sessions.size() == 1);
  janus::Session pi = janus::scan_session(pi_sessions.front());
  {
    janus::Appender output(pi_sessions.front().store);
    janus::append_pi(output, pi, {"user", "from pi", {}});
    janus::append_pi(output, pi, {"user", "repeat", {}});
    janus::append_pi(output, pi, {"user", "repeat", {}});
  }
  REQUIRE(syncer.sync() == 9);
  REQUIRE(syncer.sync() == 0);

  for (const janus::Store& store : {janus::Store{janus::Harness::claude, options.claude_root},
                                    janus::Store{janus::Harness::codex, options.codex_root},
                                    janus::Store{janus::Harness::kiss, options.kiss_root},
                                    janus::Store{janus::Harness::pi, options.pi_root}}) {
    const auto sessions = janus::discover_sessions(store);
    REQUIRE(sessions.size() == 1);
    std::size_t count = 0;
    janus::scan_session(sessions.front(), [&](const janus::Message&) { ++count; });
    REQUIRE(count == 5);
  }

  std::ifstream state(options.state);
  std::size_t rows = 0;
  for (std::string line; std::getline(state, line);) {
    REQUIRE(line.starts_with("v2\t"));
    ++rows;
  }
  REQUIRE(rows == 4);
}

TEST_CASE("sync migrates the old pair state") {
  TestDirectory temporary;
  const auto claude = write_claude_session(temporary.path() / "claude");
  const auto codex = janus::create_peer(temporary.path() / "codex", claude, janus::Harness::claude);
  const auto state = temporary.path() / "state/pairs.tsv";
  janus::fs::create_directories(state.parent_path());
  {
    std::ofstream output(state);
    output << claude.string() << '\t' << codex.string() << '\t' << janus::fs::file_size(claude)
           << '\t' << janus::fs::file_size(codex) << '\n';
  }

  janus::Syncer syncer(temporary.path() / "claude", temporary.path() / "codex", state);
  REQUIRE(syncer.sync() == 0);

  std::ifstream input(state);
  std::size_t rows = 0;
  for (std::string line; std::getline(input, line);) {
    REQUIRE(line.starts_with("v2\t"));
    ++rows;
  }
  REQUIRE(rows == 2);
}

TEST_CASE("an old group gets a peer in a newly available harness") {
  TestDirectory temporary;
  const auto claude = write_claude_session(temporary.path() / "claude");
  const auto codex = janus::create_peer(temporary.path() / "codex", claude, janus::Harness::claude);
  janus::Options options;
  options.claude_root = temporary.path() / "claude";
  options.codex_root = temporary.path() / "codex";
  options.kiss_root = temporary.path() / "kiss";
  options.pi_root = temporary.path() / "no-pi";
  options.openclaw_root = temporary.path() / "no-openclaw";
  options.hermes_db = temporary.path() / "no-hermes.db";
  options.opencode_db = temporary.path() / "no-opencode.db";
  options.state = temporary.path() / "state/pairs.tsv";
  janus::fs::create_directories(options.kiss_root);
  janus::fs::create_directories(options.state.parent_path());
  {
    std::ofstream output(options.state);
    output << claude.string() << '\t' << codex.string() << '\t' << janus::fs::file_size(claude)
           << '\t' << janus::fs::file_size(codex) << '\n';
  }

  janus::Syncer syncer(options);
  REQUIRE(syncer.sync() == 1);
  REQUIRE(janus::discover_sessions({janus::Harness::kiss, options.kiss_root}).size() == 1);
  REQUIRE(syncer.sync() == 0);
}
