#include <sqlite3.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "janus/adapters.hpp"
#include "janus/sync.hpp"
#include "janus/update.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

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

std::vector<std::string> sql_column(const janus::fs::path& path, const std::string& query) {
  sqlite3* database = nullptr;
  REQUIRE(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK);
  sqlite3_stmt* statement = nullptr;
  REQUIRE(sqlite3_prepare_v2(database, query.c_str(), -1, &statement, nullptr) == SQLITE_OK);
  std::vector<std::string> values;
  while (sqlite3_step(statement) == SQLITE_ROW) {
    values.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
  }
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return values;
}

constexpr const char* opencode_schema =
    "CREATE TABLE project(id TEXT PRIMARY KEY, worktree TEXT NOT NULL, time_updated INTEGER NOT "
    "NULL);"
    "CREATE TABLE session(id TEXT PRIMARY KEY, project_id TEXT NOT NULL, parent_id TEXT, slug "
    "TEXT NOT NULL, directory TEXT NOT NULL, title TEXT NOT NULL, version TEXT NOT NULL, "
    "time_created INTEGER NOT NULL, time_updated INTEGER NOT NULL, time_archived INTEGER);"
    "CREATE TABLE message(id TEXT PRIMARY KEY, session_id TEXT NOT NULL, time_created INTEGER "
    "NOT NULL, time_updated INTEGER NOT NULL, data TEXT NOT NULL);"
    "CREATE TABLE part(id TEXT PRIMARY KEY, message_id TEXT NOT NULL, session_id TEXT NOT NULL, "
    "time_created INTEGER NOT NULL, time_updated INTEGER NOT NULL, data TEXT NOT NULL);";

constexpr const char* hermes_schema =
    "CREATE TABLE sessions(id TEXT PRIMARY KEY, source TEXT NOT NULL, started_at REAL NOT NULL, "
    "message_count INTEGER DEFAULT 0, tool_call_count INTEGER DEFAULT 0, cwd TEXT, title TEXT, "
    "title_source TEXT, last_activity_at REAL, archived INTEGER DEFAULT 0, hidden INTEGER "
    "DEFAULT 0);"
    "CREATE TABLE messages(id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL, role "
    "TEXT NOT NULL, content TEXT, timestamp REAL NOT NULL, active INTEGER DEFAULT 1);";

constexpr const char* openclaw_schema =
    "CREATE TABLE session_nodes(session_key TEXT PRIMARY KEY, current_session_id TEXT NOT NULL, "
    "entry_json TEXT NOT NULL, entry_valid INTEGER NOT NULL, updated_at INTEGER NOT NULL, "
    "created_at INTEGER, created_via TEXT, created_actor_type TEXT, display_name TEXT, label "
    "TEXT);"
    "CREATE TABLE session_windows(session_id TEXT PRIMARY KEY, session_key TEXT NOT NULL, "
    "session_scope TEXT NOT NULL, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, "
    "started_at INTEGER, ended_at INTEGER, transcript_updated_at INTEGER, display_name TEXT);"
    "CREATE TABLE transcript_events(session_id TEXT NOT NULL, seq INTEGER NOT NULL, event_json "
    "TEXT NOT NULL, created_at INTEGER NOT NULL, PRIMARY KEY(session_id, seq));"
    "CREATE TABLE transcript_rewrite_watermarks(session_id TEXT PRIMARY KEY, generation TEXT NOT "
    "NULL, updated_at INTEGER NOT NULL);";

janus::SessionRef source_ref(const janus::fs::path& path) {
  return {janus::Harness::claude, path, "source", janus::fs::file_size(path)};
}

janus::Options two_store_options(const janus::fs::path& root) {
  janus::Options options;
  options.claude_root = root / "claude";
  options.codex_root = root / "codex/sessions";
  options.kiss_root = root / "no-kiss";
  options.pi_root = root / "no-pi";
  options.openclaw_root = root / "no-openclaw";
  options.hermes_db = root / "no-hermes.db";
  options.opencode_db = root / "no-opencode.db";
  options.state = root / "state/pairs.tsv";
  janus::fs::create_directories(options.claude_root);
  janus::fs::create_directories(options.codex_root);
  return options;
}

janus::fs::path write_named_session(const janus::fs::path& directory, const std::string& id) {
  janus::fs::create_directories(directory);
  const janus::fs::path path = directory / (id + ".jsonl");
  std::ofstream output(path);
  output << "{\"type\":\"user\",\"uuid\":\"" << id << "\",\"sessionId\":\"" << id
         << "\",\"cwd\":\"/tmp/work\",\"timestamp\":\"2026-01-01T00:00:00Z\",\"message\":{"
            "\"role\":\"user\",\"content\":\"hello "
         << id << "\"}}\n";
  return path;
}

void add_message(const janus::SessionRef& session, const std::string& text) {
  janus::Session target = janus::scan_session(session);
  janus::Appender output(session.store);
  if (session.harness == janus::Harness::codex) {
    janus::append_codex(output, target, {"user", text, {}});
  } else {
    janus::append_claude(output, target, {"user", text, {}});
  }
}

std::size_t message_count(const janus::SessionRef& session) {
  std::size_t count = 0;
  janus::scan_session(session, [&](const janus::Message&) { ++count; });
  return count;
}

std::size_t session_count(janus::Harness harness, const janus::fs::path& store) {
  return janus::discover_sessions({harness, store}).size();
}

std::string file_text(const janus::fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::stringstream text;
  text << input.rdbuf();
  return text.str();
}

}  // namespace

TEST_CASE("JSON strings are escaped") { REQUIRE(janus::quote("a\n\"b\\") == "\"a\\n\\\"b\\\\\""); }

TEST_CASE("process lock excludes another owner") {
  TestDirectory temporary;
  const auto state = temporary.path() / "state/pairs.tsv";
  {
    janus::ProcessLock first(state);
    REQUIRE_THROWS_AS(janus::ProcessLock(state), std::runtime_error);
  }
  REQUIRE_NOTHROW(janus::ProcessLock(state));
}

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
  sql(database, hermes_schema);
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
  sql(database, opencode_schema);
  sql(database, "INSERT INTO project VALUES('global','/',1);");
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
  sql(database, openclaw_schema);
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

TEST_CASE("sync removes a group only after all its session files are deleted") {
  TestDirectory temporary;
  const auto claude_root = temporary.path() / "claude";
  const auto codex_root = temporary.path() / "codex";
  const auto source = write_claude_session(claude_root);
  const auto state = temporary.path() / "state/pairs.tsv";
  janus::Syncer syncer(claude_root, codex_root, state);
  REQUIRE(syncer.sync() == 1);
  const auto peers = janus::discover_sessions({janus::Harness::codex, codex_root});
  REQUIRE(peers.size() == 1);

  janus::fs::remove(source);
  REQUIRE(syncer.sync() == 0);
  REQUIRE(janus::fs::file_size(state) > 0);

  janus::fs::remove(peers.front().store);
  REQUIRE(syncer.sync() == 0);
  REQUIRE(janus::fs::file_size(state) == 0);

  write_claude_session(claude_root);
  REQUIRE(syncer.sync() == 1);
  REQUIRE(janus::discover_sessions({janus::Harness::codex, codex_root}).size() == 1);
}

TEST_CASE("sync does not duplicate peers when one target store fails") {
  TestDirectory temporary;
  janus::Options options;
  options.claude_root = temporary.path() / "claude";
  options.codex_root = temporary.path() / "codex";
  options.kiss_root = temporary.path() / "no-kiss";
  options.pi_root = temporary.path() / "no-pi";
  options.openclaw_root = temporary.path() / "no-openclaw";
  options.hermes_db = temporary.path() / "no-hermes.db";
  options.opencode_db = temporary.path() / "opencode.db";
  options.state = temporary.path() / "state/pairs.tsv";
  write_claude_session(options.claude_root);
  janus::fs::create_directories(options.codex_root);
  sql(options.opencode_db, opencode_schema);

  janus::Syncer syncer(options);
  REQUIRE(syncer.sync() == 1);
  REQUIRE(syncer.sync() == 0);
  REQUIRE(janus::discover_sessions({janus::Harness::codex, options.codex_root}).size() == 1);

  sql(options.opencode_db, "INSERT INTO project VALUES('global','/',1);");
  REQUIRE(syncer.sync() == 1);
  REQUIRE(janus::discover_sessions({janus::Harness::opencode, options.opencode_db}).size() == 1);
  REQUIRE(janus::discover_sessions({janus::Harness::codex, options.codex_root}).size() == 1);
}

TEST_CASE("import does not duplicate peers when one target store fails") {
  TestDirectory temporary;
  janus::Options options;
  options.claude_root = temporary.path() / "claude";
  options.codex_root = temporary.path() / "codex";
  options.kiss_root = temporary.path() / "no-kiss";
  options.pi_root = temporary.path() / "no-pi";
  options.openclaw_root = temporary.path() / "no-openclaw";
  options.hermes_db = temporary.path() / "no-hermes.db";
  options.opencode_db = temporary.path() / "opencode.db";
  options.state = temporary.path() / "state/pairs.tsv";
  const auto source = write_claude_session(options.claude_root);
  janus::fs::create_directories(options.codex_root);
  sql(options.opencode_db, opencode_schema);

  const auto codex = janus::Syncer(options).import_one(source, janus::Harness::claude);
  REQUIRE(janus::Syncer(options).import_one(source, janus::Harness::claude) == codex);
  REQUIRE(janus::discover_sessions({janus::Harness::codex, options.codex_root}).size() == 1);

  sql(options.opencode_db, "INSERT INTO project VALUES('global','/',1);");
  REQUIRE(janus::Syncer(options).import_one(source, janus::Harness::claude) == codex);
  REQUIRE(janus::discover_sessions({janus::Harness::opencode, options.opencode_db}).size() == 1);
  REQUIRE(janus::discover_sessions({janus::Harness::codex, options.codex_root}).size() == 1);
}

TEST_CASE("import reports an error when every target store fails") {
  TestDirectory temporary;
  janus::Options options;
  options.claude_root = temporary.path() / "claude";
  options.codex_root = temporary.path() / "no-codex";
  options.kiss_root = temporary.path() / "no-kiss";
  options.pi_root = temporary.path() / "no-pi";
  options.openclaw_root = temporary.path() / "no-openclaw";
  options.hermes_db = temporary.path() / "no-hermes.db";
  options.opencode_db = temporary.path() / "opencode.db";
  options.state = temporary.path() / "state/pairs.tsv";
  const auto source = write_claude_session(options.claude_root);
  sql(options.opencode_db, opencode_schema);

  REQUIRE_THROWS_AS(janus::Syncer(options).import_one(source, janus::Harness::claude),
                    std::runtime_error);
  sql(options.opencode_db, "INSERT INTO project VALUES('global','/',1);");
  const auto peer = janus::Syncer(options).import_one(source, janus::Harness::claude);
  REQUIRE(peer == options.opencode_db);
  REQUIRE(janus::discover_sessions({janus::Harness::opencode, options.opencode_db}).size() == 1);
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

TEST_CASE("a bad state row stops sync and changes nothing") {
  TestDirectory temporary;
  const janus::Options options = two_store_options(temporary.path());
  write_named_session(options.claude_root, "s1");
  write_named_session(options.claude_root, "s2");
  janus::Syncer(options).sync();
  {
    std::ofstream state(options.state, std::ios::app);
    state << "v2\tgroup\tfuture-harness\t/nowhere\tid\t0\n";
  }

  janus::Syncer live(options);
  for (int tick = 0; tick < 2; ++tick) {
    REQUIRE_THROWS_WITH(live.sync(), Catch::Matchers::ContainsSubstring("line 5"));
  }
  REQUIRE(session_count(janus::Harness::claude, options.claude_root) == 2);
  REQUIRE(session_count(janus::Harness::codex, options.codex_root) == 2);
  REQUIRE(file_text(options.state).find("future-harness") != std::string::npos);
}

TEST_CASE("new peers stay recorded when the final state save fails") {
  TestDirectory temporary;
  const janus::Options options = two_store_options(temporary.path());
  write_named_session(options.claude_root, "s1");
  const janus::fs::path blocked_save = options.state.string() + ".tmp";
  janus::fs::create_directories(blocked_save);
  REQUIRE_THROWS(janus::Syncer(options).sync());
  janus::fs::remove_all(blocked_save);
  {
    std::ofstream state(options.state, std::ios::app);
    state << "v2\tunfinished";
  }

  REQUIRE(janus::Syncer(options).sync() == 0);
  REQUIRE(session_count(janus::Harness::claude, options.claude_root) == 1);
  REQUIRE(session_count(janus::Harness::codex, options.codex_root) == 1);
  REQUIRE(file_text(options.state).find("unfinished") == std::string::npos);
}

TEST_CASE("a failed copy does not duplicate a new peer") {
  TestDirectory temporary;
  janus::Options options = two_store_options(temporary.path());
  options.codex_root = temporary.path() / "no-codex";
  options.opencode_db = temporary.path() / "opencode.db";
  sql(options.opencode_db, opencode_schema);
  sql(options.opencode_db, "INSERT INTO project VALUES('global','/',1);");
  const auto source = write_claude_session(options.claude_root);
  janus::Syncer(options).sync();
  const auto opencode =
      janus::discover_sessions({janus::Harness::opencode, options.opencode_db}).front();

  sql(options.opencode_db, "DROP TABLE part;");
  add_message(source_ref(source), "new message");
  options.kiss_root = temporary.path() / "kiss";
  janus::fs::create_directories(options.kiss_root);
  for (int run = 0; run < 3; ++run) {
    REQUIRE_THROWS(janus::Syncer(options).sync());
    REQUIRE(session_count(janus::Harness::kiss, options.kiss_root) == 1);
  }

  sql(options.opencode_db,
      "CREATE TABLE part(id TEXT PRIMARY KEY, message_id TEXT NOT NULL, session_id TEXT NOT NULL, "
      "time_created INTEGER NOT NULL, time_updated INTEGER NOT NULL, data TEXT NOT NULL);");
  janus::Syncer(options).sync();
  REQUIRE(session_count(janus::Harness::claude, options.claude_root) == 1);
  REQUIRE(session_count(janus::Harness::kiss, options.kiss_root) == 1);
  REQUIRE(session_count(janus::Harness::opencode, options.opencode_db) == 1);
  REQUIRE(file_text(options.state).find(opencode.id) != std::string::npos);
}

TEST_CASE("a failed Codex index append leaves no peer file") {
  TestDirectory temporary;
  const janus::Options options = two_store_options(temporary.path());
  write_named_session(options.claude_root, "s1");
  const janus::fs::path blocked_index = options.codex_root.parent_path() / "session_index.jsonl";
  janus::fs::create_directories(blocked_index);

  janus::Syncer live(options);
  for (int tick = 0; tick < 3; ++tick) {
    REQUIRE(live.sync() == 0);
    REQUIRE(session_count(janus::Harness::claude, options.claude_root) == 1);
    REQUIRE(session_count(janus::Harness::codex, options.codex_root) == 0);
  }

  janus::fs::remove_all(blocked_index);
  REQUIRE(live.sync() == 1);
  REQUIRE(session_count(janus::Harness::claude, options.claude_root) == 1);
  REQUIRE(session_count(janus::Harness::codex, options.codex_root) == 1);
}

TEST_CASE("a deleted origin does not stop its group or other groups") {
  TestDirectory temporary;
  const janus::Options options = two_store_options(temporary.path());
  const auto origin = write_named_session(options.claude_root, "s1");
  janus::Syncer live(options);
  live.sync();
  const auto peer = janus::discover_sessions({janus::Harness::codex, options.codex_root}).front();
  janus::fs::remove(origin);
  add_message(peer, "continue in Codex");

  REQUIRE_NOTHROW(live.sync());
  write_named_session(options.claude_root, "s2");
  REQUIRE(live.sync() == 1);
  REQUIRE(session_count(janus::Harness::claude, options.claude_root) == 1);
  REQUIRE(session_count(janus::Harness::codex, options.codex_root) == 2);
  REQUIRE(file_text(options.state).find(peer.id) != std::string::npos);
}

TEST_CASE("a deleted peer stays deleted after a restart until an import") {
  TestDirectory temporary;
  const janus::Options options = two_store_options(temporary.path());
  const auto source = write_named_session(options.claude_root, "s1");
  janus::Syncer(options).sync();
  janus::fs::remove(
      janus::discover_sessions({janus::Harness::codex, options.codex_root}).front().store);

  REQUIRE(janus::Syncer(options).sync() == 0);
  REQUIRE(session_count(janus::Harness::codex, options.codex_root) == 0);

  const auto imported = janus::Syncer(options).import_one(source, janus::Harness::claude);
  REQUIRE(janus::fs::exists(imported));
  REQUIRE(janus::Syncer(options).sync() == 0);
  REQUIRE(session_count(janus::Harness::codex, options.codex_root) == 1);
}

TEST_CASE("an inactive database session gets no writes and catches up after a restore") {
  struct Case {
    janus::Harness harness;
    std::string schema;
    const char* deactivate;
    const char* restore;
  };
  const std::string project = "INSERT INTO project VALUES('global','/',1);";
  for (const Case& entry :
       {Case{janus::Harness::hermes, hermes_schema, "UPDATE sessions SET archived = 1",
             "UPDATE sessions SET archived = 0"},
        Case{janus::Harness::hermes, hermes_schema, "UPDATE sessions SET hidden = 1",
             "UPDATE sessions SET hidden = 0"},
        Case{janus::Harness::opencode, opencode_schema + project,
             "UPDATE session SET time_archived = 1", "UPDATE session SET time_archived = NULL"},
        Case{janus::Harness::openclaw, openclaw_schema, "UPDATE session_windows SET ended_at = 1",
             "UPDATE session_windows SET ended_at = NULL"}}) {
    TestDirectory temporary;
    janus::Options options = two_store_options(temporary.path());
    options.codex_root = temporary.path() / "no-codex";
    const janus::fs::path database = temporary.path() / "store.db";
    if (entry.harness == janus::Harness::hermes) options.hermes_db = database;
    if (entry.harness == janus::Harness::opencode) options.opencode_db = database;
    if (entry.harness == janus::Harness::openclaw) options.openclaw_root = database;
    sql(database, entry.schema.c_str());
    const auto source = write_claude_session(options.claude_root);
    janus::Syncer live(options);
    REQUIRE(live.sync() == 1);
    const auto peer = janus::discover_sessions({entry.harness, database}).front();

    sql(database, entry.deactivate);
    add_message(source_ref(source), "while inactive");
    REQUIRE(live.sync() == 0);

    sql(database, entry.restore);
    REQUIRE(message_count(peer) == 2);
    REQUIRE(live.sync() == 1);
    REQUIRE(message_count(peer) == 3);
    REQUIRE(session_count(janus::Harness::claude, options.claude_root) == 1);
  }
}

TEST_CASE("a group with only archived members survives until the restore") {
  TestDirectory temporary;
  janus::Options options = two_store_options(temporary.path());
  options.codex_root = temporary.path() / "no-codex";
  options.hermes_db = temporary.path() / "hermes.db";
  options.opencode_db = temporary.path() / "opencode.db";
  sql(options.hermes_db, hermes_schema);
  sql(options.opencode_db, opencode_schema);
  sql(options.opencode_db, "INSERT INTO project VALUES('global','/',1);");
  const auto source = write_claude_session(options.claude_root);
  janus::Syncer live(options);
  REQUIRE(live.sync() == 2);

  janus::fs::remove(source);
  sql(options.hermes_db, "UPDATE sessions SET archived = 1");
  sql(options.opencode_db, "UPDATE session SET time_archived = 1");
  REQUIRE(live.sync() == 0);
  sql(options.hermes_db, "UPDATE sessions SET archived = 0");
  sql(options.opencode_db, "UPDATE session SET time_archived = NULL");
  REQUIRE(live.sync() == 0);
  REQUIRE(session_count(janus::Harness::hermes, options.hermes_db) == 1);
  REQUIRE(session_count(janus::Harness::opencode, options.opencode_db) == 1);
}

TEST_CASE("a deleted database row does not stop the group") {
  TestDirectory temporary;
  janus::Options options = two_store_options(temporary.path());
  options.codex_root = temporary.path() / "no-codex";
  options.hermes_db = temporary.path() / "hermes.db";
  sql(options.hermes_db, hermes_schema);
  const auto source = write_claude_session(options.claude_root);
  janus::Syncer live(options);
  live.sync();

  sql(options.hermes_db, "DELETE FROM sessions; DELETE FROM messages;");
  add_message(source_ref(source), "after delete");
  REQUIRE(live.sync() == 0);
  REQUIRE(session_count(janus::Harness::hermes, options.hermes_db) == 0);
}

TEST_CASE("an append never joins a record to an unterminated line") {
  TestDirectory temporary;
  const auto source = write_claude_session(temporary.path() / "claude");
  const auto target = temporary.path() / "target.jsonl";
  const std::string complete =
      "{\"type\":\"user\",\"uuid\":\"t1\",\"sessionId\":\"target\",\"message\":{\"role\":\"user\","
      "\"content\":\"complete\"}}";
  const janus::SessionRef target_ref{janus::Harness::claude, target, "target", 0};
  const auto write_tail = [&](const std::string& tail) {
    std::ofstream output(target, std::ios::binary);
    output << complete << '\n' << tail;
  };
  const auto copy = [&] {
    return janus::copy_missing(source, janus::Harness::claude, target, janus::Harness::claude);
  };

  SECTION("a complete final record counts and gets its newline") {
    {
      std::ofstream output(target, std::ios::binary);
      output << complete;
    }
    REQUIRE(message_count(target_ref) == 1);
    REQUIRE(copy() == 2);
    REQUIRE(message_count(target_ref) == 3);
  }
  SECTION("a record in progress defers the copy and leaves the file unchanged") {
    write_tail("{\"type\":\"user\",\"uuid\":\"t2");
    const std::string before = file_text(target);
    REQUIRE_THROWS_WITH(copy(), Catch::Matchers::ContainsSubstring("unfinished record"));
    REQUIRE(file_text(target) == before);
  }
  SECTION("an abandoned partial record stays a skipped line") {
    write_tail("{\"type\":\"user\",\"uuid\":\"t2");
    janus::fs::last_write_time(target,
                               janus::fs::file_time_type::clock::now() - std::chrono::hours(1));
    REQUIRE(copy() == 2);
    REQUIRE(message_count(target_ref) == 3);
  }
}

TEST_CASE("one failing group does not stop other groups") {
  TestDirectory temporary;
  const janus::Options options = two_store_options(temporary.path());
  const auto s1 = write_named_session(options.claude_root, "s1");
  const auto s2 = write_named_session(options.claude_root, "s2");
  janus::Syncer live(options);
  live.sync();
  const auto peer_of = [&](const std::string& text) {
    for (const janus::SessionRef& peer :
         janus::discover_sessions({janus::Harness::codex, options.codex_root})) {
      if (janus::scan_session(peer).first_user == text) return peer;
    }
    throw std::runtime_error("no peer has first message: " + text);
  };
  const auto peer1 = peer_of("hello s1");
  const auto peer2 = peer_of("hello s2");
  {
    std::ofstream output(peer1.store, std::ios::binary | std::ios::app);
    output << "{\"partial\":";
  }
  add_message({janus::Harness::claude, s1, "s1", 0}, "one");
  add_message({janus::Harness::claude, s2, "s2", 0}, "two");

  REQUIRE_THROWS(live.sync());
  REQUIRE(message_count(peer1) == 1);
  REQUIRE(message_count(peer2) == 2);

  {
    std::ofstream output(peer1.store, std::ios::binary | std::ios::app);
    output << "null}\n";
  }
  REQUIRE(live.sync() == 1);
  REQUIRE(message_count(peer1) == 2);
}

TEST_CASE("a store that appears during serve is used without a restart") {
  TestDirectory temporary;
  const janus::Options options = two_store_options(temporary.path());
  write_named_session(options.claude_root, "s1");
  janus::Syncer live(options);
  REQUIRE(live.sync() == 1);
  janus::fs::create_directories(options.kiss_root);
  REQUIRE(live.sync() == 1);
  REQUIRE(session_count(janus::Harness::kiss, options.kiss_root) == 1);
}

TEST_CASE("a session that cannot be read does not look empty") {
  TestDirectory temporary;
  const auto directory = temporary.path() / "session.jsonl";
  janus::fs::create_directories(directory);
  REQUIRE_THROWS(janus::scan_session(directory, janus::Harness::claude));
}

TEST_CASE("the session cursor is the last record in the parent chain") {
  TestDirectory temporary;

  SECTION("Claude counts records without text and skips sidechains") {
    const auto path = temporary.path() / "claude.jsonl";
    {
      std::ofstream output(path);
      output << "{\"type\":\"user\",\"uuid\":\"u1\",\"sessionId\":\"s\",\"message\":{\"role\":"
                "\"user\",\"content\":\"hello\"}}\n"
             << "{\"type\":\"assistant\",\"uuid\":\"u2\",\"sessionId\":\"s\",\"message\":{\"role\":"
                "\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"x\"}]}}\n"
             << "{\"type\":\"user\",\"uuid\":\"u3\",\"isSidechain\":true,\"sessionId\":\"s\","
                "\"message\":{\"role\":\"user\",\"content\":\"side\"}}\n";
    }
    REQUIRE(janus::scan_session(path, janus::Harness::claude).last_uuid == "u2");
  }
  SECTION("Pi counts records of other roles and types, but not the header") {
    const auto path = temporary.path() / "pi.jsonl";
    {
      std::ofstream output(path);
      output << "{\"type\":\"session\",\"id\":\"session-id\",\"cwd\":\"/tmp\"}\n";
    }
    REQUIRE(janus::scan_session(path, janus::Harness::pi).last_uuid.empty());
    {
      std::ofstream output(path, std::ios::app);
      output << "{\"type\":\"message\",\"id\":\"m1\",\"message\":{\"role\":\"user\",\"content\":"
                "\"hello\"}}\n"
             << "{\"type\":\"message\",\"id\":\"m2\",\"message\":{\"role\":\"toolResult\","
                "\"content\":\"done\"}}\n";
    }
    REQUIRE(janus::scan_session(path, janus::Harness::pi).last_uuid == "m2");
  }
  SECTION("OpenCode reads the newest message row") {
    const auto source = write_claude_session(temporary.path() / "claude");
    const auto database = temporary.path() / "opencode.db";
    sql(database, opencode_schema);
    sql(database, "INSERT INTO project VALUES('global','/',1);");
    const auto peer = janus::create_peer({janus::Harness::opencode, database}, source_ref(source));
    REQUIRE_FALSE(janus::scan_session(peer).last_uuid.empty());
    sql(database, ("INSERT INTO message VALUES('msg_tool_only','" + peer.id +
                   "',9999999999999,9999999999999,'{\"role\":\"assistant\"}');")
                      .c_str());
    REQUIRE(janus::scan_session(peer).last_uuid == "msg_tool_only");
  }
  SECTION("OpenClaw counts events other than messages") {
    const auto source = write_claude_session(temporary.path() / "claude");
    const auto database = temporary.path() / "openclaw.db";
    sql(database, openclaw_schema);
    const auto peer = janus::create_peer({janus::Harness::openclaw, database}, source_ref(source));
    sql(database, ("INSERT INTO transcript_events VALUES('" + peer.id +
                   "',99,'{\"type\":\"compaction\",\"id\":\"e99\"}',0);")
                      .c_str());
    REQUIRE(janus::scan_session(peer).last_uuid == "e99");
  }
}

#ifndef _WIN32
TEST_CASE("a session path with a tab stays out of the state file") {
  TestDirectory temporary;
  const janus::Options options = two_store_options(temporary.path());
  const auto unsafe = write_named_session(options.claude_root / "with\ttab", "s1");
  write_named_session(options.claude_root / "plain", "s2");

  REQUIRE(janus::Syncer(options).sync() == 1);
  REQUIRE(janus::Syncer(options).sync() == 0);
  REQUIRE(session_count(janus::Harness::codex, options.codex_root) == 1);
  REQUIRE_THROWS(janus::Syncer(options).import_one(unsafe, janus::Harness::claude));
}
#endif

TEST_CASE("a message that arrives during a sync is not lost") {
  TestDirectory temporary;
  const janus::Options options = two_store_options(temporary.path());
  const auto source = write_named_session(options.claude_root, "s1");
  const janus::SessionRef source_session{janus::Harness::claude, source, "s1", 0};
  {
    // A larger session gives each sync enough duration for a writer to land inside it.
    janus::Session target = janus::scan_session(source_session);
    janus::Appender output(source);
    for (int index = 0; index < 2000; ++index) {
      janus::append_claude(output, target, {"user", "seed " + std::to_string(index), {}});
    }
  }
  janus::Syncer live(options);
  live.sync();
  const auto peer = janus::discover_sessions({janus::Harness::codex, options.codex_root}).front();
  const std::size_t seeded = message_count(peer);

  std::chrono::steady_clock::duration last_sync{};
  for (std::size_t round = 1; round <= 60; ++round) {
    add_message(source_session, "before " + std::to_string(round));
    const auto delay = last_sync * static_cast<int>(round % 10) / 10;
    std::thread writer([&, delay, round] {
      std::this_thread::sleep_for(delay);
      add_message(source_session, "during " + std::to_string(round));
    });
    const auto start = std::chrono::steady_clock::now();
    live.sync();
    last_sync = std::chrono::steady_clock::now() - start;
    writer.join();
    live.sync();
    REQUIRE(message_count(peer) == seeded + 2 * round);
  }
}

TEST_CASE("version order decides whether an update can run") {
  REQUIRE(janus::compare_versions("0.0.3", "0.0.3") == 0);
  REQUIRE(janus::compare_versions("0.0.10", "0.0.9") > 0);
  REQUIRE(janus::compare_versions("0.9.0", "0.10.0") < 0);
  REQUIRE(janus::compare_versions("1.0", "1.0.0") == 0);
  REQUIRE(janus::compare_versions("1.0.0-rc1", "1.0.0") < 0);
  REQUIRE(janus::compare_versions("1.0.0", "1.0.0-rc1") > 0);
  REQUIRE(janus::compare_versions("1.0.0-rc2", "1.0.0-rc1") > 0);
  REQUIRE(janus::compare_versions("1.0.0+build", "1.0.0") == 0);
  REQUIRE_FALSE(janus::compare_versions("main", "1.0.0").has_value());
  REQUIRE_FALSE(janus::compare_versions("1.0.0", "1..0").has_value());
}

TEST_CASE("OpenCode messages keep their source order") {
  TestDirectory temporary;
  const auto source = temporary.path() / "source.jsonl";
  {
    janus::Session target{"source", "/tmp/work", {}, 0, {}, {}};
    janus::Appender output(source);
    for (int index = 0; index < 200; ++index) {
      janus::append_claude(output, target, {"user", "m" + std::to_string(index), {}});
    }
  }
  const auto database = temporary.path() / "opencode.db";
  sql(database, opencode_schema);
  sql(database, "INSERT INTO project VALUES('global','/',1);");
  const auto peer = janus::create_peer({janus::Harness::opencode, database}, source_ref(source));
  const auto read_texts = [&] {
    std::vector<std::string> texts;
    janus::scan_session(peer,
                        [&](const janus::Message& message) { texts.push_back(message.text); });
    return texts;
  };

  std::vector<std::string> texts = read_texts();
  REQUIRE(texts.size() == 200);
  for (std::size_t index = 0; index < texts.size(); ++index) {
    REQUIRE(texts[index] == "m" + std::to_string(index));
  }
  // OpenCode also sorts by ID, so the ID order must match the time order.
  REQUIRE(sql_column(database, "SELECT id FROM message ORDER BY time_created, id") ==
          sql_column(database, "SELECT id FROM message ORDER BY id"));

  // A new message follows the newest row, even if that row has a later time than the clock.
  sql(database,
      ("INSERT INTO message VALUES('msg_ffffffffffff','" + peer.id +
       "',9999999999999,9999999999999,'{\"role\":\"user\"}');"
       "INSERT INTO part VALUES('prt_ffffffffffff','msg_ffffffffffff','" +
       peer.id + "',9999999999999,9999999999999,'{\"type\":\"text\",\"text\":\"future\"}');")
          .c_str());
  add_message(source_ref(source), "after future");
  janus::copy_missing(source_ref(source), peer);
  texts = read_texts();
  REQUIRE(texts.back() == "after future");
}

#ifndef _WIN32
TEST_CASE("an unreadable new session does not stop other new sessions in its store") {
  if (::geteuid() == 0) SKIP("root can read a file with no permissions");
  TestDirectory temporary;
  const janus::Options options = two_store_options(temporary.path());
  write_named_session(options.claude_root, "readable");
  const auto blocked = write_named_session(options.claude_root, "unreadable");
  janus::fs::permissions(blocked, janus::fs::perms::none);

  janus::Syncer live(options);
  REQUIRE_THROWS(live.sync());
  REQUIRE(session_count(janus::Harness::codex, options.codex_root) == 1);

  janus::fs::permissions(blocked, janus::fs::perms::owner_read | janus::fs::perms::owner_write);
  REQUIRE(live.sync() == 1);
  REQUIRE(session_count(janus::Harness::codex, options.codex_root) == 2);
}
#endif

TEST_CASE("discovery parses a file again only after its size changes") {
  TestDirectory temporary;
  const auto path = write_named_session(temporary.path() / "claude", "s1");
  const janus::Store store{janus::Harness::claude, temporary.path() / "claude"};
  janus::Discovery discovery;
  REQUIRE(janus::discover_sessions(store, &discovery).size() == 1);
  REQUIRE(janus::discover_sessions(store, &discovery).empty());
  add_message({janus::Harness::claude, path, "s1", 0}, "more");
  REQUIRE(janus::discover_sessions(store, &discovery).size() == 1);
}

TEST_CASE("a session with no messages joins a group after it gets its first message") {
  TestDirectory temporary;
  const janus::Options options = two_store_options(temporary.path());
  const auto path = options.claude_root / "s1.jsonl";
  {
    std::ofstream output(path);
    output << "{\"type\":\"ai-title\",\"sessionId\":\"s1\",\"aiTitle\":\"title\"}\n";
  }
  janus::Syncer live(options);
  REQUIRE(live.sync() == 0);
  REQUIRE(live.sync() == 0);
  add_message({janus::Harness::claude, path, "s1", 0}, "first");
  REQUIRE(live.sync() == 1);
}
