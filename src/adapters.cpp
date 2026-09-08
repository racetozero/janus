#include "janus/adapters.hpp"

#include <simdjson.h>
#include <sqlite3.h>
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <memory>
#include <unordered_map>

namespace janus {

std::string_view harness_name(Harness harness) {
  switch (harness) {
    case Harness::claude:
      return "claude";
    case Harness::codex:
      return "codex";
    case Harness::kiss:
      return "kiss";
    case Harness::pi:
      return "pi";
    case Harness::openclaw:
      return "openclaw";
    case Harness::hermes:
      return "hermes";
    case Harness::opencode:
      return "opencode";
  }
  throw std::runtime_error("unknown harness");
}

Harness parse_harness(std::string_view name) {
  for (const Harness harness : {Harness::claude, Harness::codex, Harness::kiss, Harness::pi,
                                Harness::openclaw, Harness::hermes, Harness::opencode}) {
    if (name == harness_name(harness)) return harness;
  }
  throw std::runtime_error("unknown harness: " + std::string(name));
}

namespace {

class Statement {
 public:
  Statement(sqlite3* database, const char* sql) : database_(database) {
    if (sqlite3_prepare_v2(database, sql, -1, &value_, nullptr) != SQLITE_OK) fail();
  }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  ~Statement() { sqlite3_finalize(value_); }

  void bind(int index, std::string_view value) {
    if (sqlite3_bind_text(value_, index, value.data(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK)
      fail();
  }
  void bind(int index, std::int64_t value) {
    if (sqlite3_bind_int64(value_, index, value) != SQLITE_OK) fail();
  }
  bool row() {
    const int result = sqlite3_step(value_);
    if (result == SQLITE_ROW) return true;
    if (result != SQLITE_DONE) fail();
    return false;
  }
  std::string text(int column) const {
    const unsigned char* value = sqlite3_column_text(value_, column);
    return value == nullptr ? std::string{} : reinterpret_cast<const char*>(value);
  }
  std::int64_t integer(int column) const { return sqlite3_column_int64(value_, column); }

 private:
  [[noreturn]] void fail() const { throw std::runtime_error(sqlite3_errmsg(database_)); }
  sqlite3* database_ = nullptr;
  sqlite3_stmt* value_ = nullptr;
};

class Database {
 public:
  explicit Database(const fs::path& path) : path_(path) {
    if (sqlite3_open_v2(path.string().c_str(), &value_, SQLITE_OPEN_READWRITE, nullptr) !=
        SQLITE_OK) {
      const std::string message =
          value_ == nullptr ? "cannot open database" : sqlite3_errmsg(value_);
      sqlite3_close(value_);
      value_ = nullptr;
      throw std::runtime_error(message + ": " + path.string());
    }
    sqlite3_busy_timeout(value_, 5000);
  }
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  ~Database() { sqlite3_close(value_); }

  Statement prepare(const char* sql) { return Statement(value_, sql); }
  void execute(const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(value_, sql, nullptr, nullptr, &error) == SQLITE_OK) return;
    const std::string message = error == nullptr ? sqlite3_errmsg(value_) : error;
    sqlite3_free(error);
    throw std::runtime_error(message + ": " + path_.string());
  }
  sqlite3* get() const { return value_; }

 private:
  fs::path path_;
  sqlite3* value_ = nullptr;
};

class Transaction {
 public:
  explicit Transaction(Database& database) : database_(database) {
    database_.execute("BEGIN IMMEDIATE");
  }
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  ~Transaction() {
    if (!committed_) {
      try {
        database_.execute("ROLLBACK");
      } catch (...) {
      }
    }
  }
  void commit() {
    database_.execute("COMMIT");
    committed_ = true;
  }

 private:
  Database& database_;
  bool committed_ = false;
};

std::int64_t now_milliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool is_database_harness(Harness harness) {
  return harness == Harness::openclaw || harness == Harness::hermes || harness == Harness::opencode;
}

void require_table(Database& database, std::string_view name) {
  Statement query =
      database.prepare("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ? LIMIT 1");
  query.bind(1, name);
  if (!query.row()) throw std::runtime_error("required table is missing: " + std::string(name));
}

void append_database_message(Database& database, const SessionRef& target, Session& state,
                             const Message& message) {
  const std::int64_t now = now_milliseconds();
  const std::string role = message.role == "assistant" ? "assistant" : "user";
  if (target.harness == Harness::hermes) {
    Statement insert = database.prepare(
        "INSERT INTO messages (session_id, role, content, timestamp, active) VALUES (?, ?, ?, ?, "
        "1)");
    insert.bind(1, target.id);
    insert.bind(2, role);
    insert.bind(3, message.text);
    insert.bind(4, now / 1000);
    insert.row();
    return;
  }
  if (target.harness == Harness::opencode) {
    const std::string message_id = "msg_" + make_uuid();
    const std::string part_id = "prt_" + make_uuid();
    std::string data;
    if (role == "user") {
      data = "{\"role\":\"user\",\"time\":{\"created\":" + std::to_string(now) +
             "},\"agent\":\"build\",\"model\":{\"providerID\":\"openai\","
             "\"modelID\":\"janus-import\"}}";
    } else {
      data = "{\"role\":\"assistant\",\"time\":{\"created\":" + std::to_string(now) +
             ",\"completed\":" + std::to_string(now) + "},\"parentID\":" + quote(state.last_uuid) +
             ",\"modelID\":\"janus-import\",\"providerID\":\"openai\","
             "\"mode\":\"build\",\"agent\":\"build\",\"path\":{\"cwd\":" +
             quote(state.cwd) + ",\"root\":" + quote(state.cwd) +
             "},\"cost\":0,\"tokens\":{\"input\":0,\"output\":0,\"reasoning\":0,"
             "\"cache\":{\"read\":0,\"write\":0}},\"finish\":\"stop\"}";
    }
    Statement insert_message = database.prepare(
        "INSERT INTO message (id, session_id, time_created, time_updated, data) VALUES (?, ?, ?, "
        "?, ?)");
    insert_message.bind(1, message_id);
    insert_message.bind(2, target.id);
    insert_message.bind(3, now);
    insert_message.bind(4, now);
    insert_message.bind(5, data);
    insert_message.row();
    Statement insert_part = database.prepare(
        "INSERT INTO part (id, message_id, session_id, time_created, time_updated, data) "
        "VALUES (?, ?, ?, ?, ?, ?)");
    insert_part.bind(1, part_id);
    insert_part.bind(2, message_id);
    insert_part.bind(3, target.id);
    insert_part.bind(4, now);
    insert_part.bind(5, now);
    insert_part.bind(6, "{\"type\":\"text\",\"text\":" + quote(message.text) + '}');
    insert_part.row();
    state.last_uuid = message_id;
    return;
  }

  const std::string id = make_uuid();
  const std::string body =
      role == "assistant"
          ? "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\","
            "\"text\":" +
                quote(message.text) +
                "}],\"provider\":\"janus\",\"model\":\"import\","
                "\"stopReason\":\"stop\",\"timestamp\":" +
                quote(now_iso()) + '}'
          : "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":" + quote(message.text) +
                "}],\"timestamp\":" + quote(now_iso()) + '}';
  const std::string event =
      "{\"type\":\"message\",\"id\":" + quote(id) +
      ",\"parentId\":" + (state.last_uuid.empty() ? "null" : quote(state.last_uuid)) +
      ",\"timestamp\":" + quote(message.timestamp.empty() ? now_iso() : message.timestamp) +
      ",\"message\":" + body + '}';
  Statement insert = database.prepare(
      "INSERT INTO transcript_events (session_id, seq, event_json, created_at) VALUES (?, ?, ?, "
      "?)");
  insert.bind(1, target.id);
  insert.bind(2, static_cast<std::int64_t>(++state.last_ordinal));
  insert.bind(3, event);
  insert.bind(4, now);
  insert.row();
  state.last_uuid = id;
}

void finish_database_append(Database& database, const SessionRef& target, std::size_t count) {
  if (count == 0) return;
  const std::int64_t now = now_milliseconds();
  if (target.harness == Harness::hermes) {
    Statement update = database.prepare(
        "UPDATE sessions SET message_count = message_count + ?, last_activity_at = ? WHERE id = ?");
    update.bind(1, static_cast<std::int64_t>(count));
    update.bind(2, now / 1000);
    update.bind(3, target.id);
    update.row();
  } else if (target.harness == Harness::opencode) {
    Statement update = database.prepare("UPDATE session SET time_updated = ? WHERE id = ?");
    update.bind(1, now);
    update.bind(2, target.id);
    update.row();
  } else {
    Statement update = database.prepare(
        "UPDATE session_windows SET updated_at = ?, transcript_updated_at = ? WHERE session_id = "
        "?");
    update.bind(1, now);
    update.bind(2, now);
    update.bind(3, target.id);
    update.row();
  }
}

std::string string_field(simdjson::dom::object object, std::string_view key) {
  std::string_view value;
  return object[key].get_string().get(value) == simdjson::SUCCESS ? std::string(value)
                                                                  : std::string{};
}

std::string json_text(simdjson::dom::element value) {
  std::string_view text;
  if (value.get_string().get(text) == simdjson::SUCCESS) return std::string(text);
  return simdjson::minify(value);
}

std::string content_text(simdjson::dom::element content) {
  std::string_view scalar;
  if (content.get_string().get(scalar) == simdjson::SUCCESS) return std::string(scalar);
  simdjson::dom::array blocks;
  if (content.get_array().get(blocks) != simdjson::SUCCESS) return {};

  std::string output;
  for (const simdjson::dom::element element : blocks) {
    simdjson::dom::object block;
    if (element.get_object().get(block) != simdjson::SUCCESS) continue;
    const std::string type = string_field(block, "type");
    std::string part;
    if (type == "text" || type == "input_text" || type == "output_text") {
      part = string_field(block, "text");
    } else if (type == "tool_use") {
      part = "[Tool call: " + string_field(block, "name") + "]";
      simdjson::dom::element input;
      if (block["input"].get(input) == simdjson::SUCCESS) part += "\n" + json_text(input);
    } else if (type == "tool_result") {
      part = "[Tool result]";
      simdjson::dom::element result;
      if (block["content"].get(result) == simdjson::SUCCESS) part += "\n" + content_text(result);
    }
    if (part.empty()) continue;
    if (!output.empty()) output += "\n\n";
    output += part;
  }
  return output;
}

std::string sanitize_cwd(std::string cwd) {
  if (cwd.empty()) cwd = fs::current_path().string();
  for (char& character : cwd) {
    const auto byte = static_cast<unsigned char>(character);
    if (!(std::isalnum(byte) || character == '-' || character == '_')) character = '-';
  }
  return cwd;
}

fs::path codex_path(const fs::path& root, const std::string& id) {
  const std::time_t seconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &seconds);
#else
  localtime_r(&seconds, &local);
#endif
  std::array<char, 16> directories{};
  std::array<char, 32> stamp{};
  std::strftime(directories.data(), directories.size(), "%Y/%m/%d", &local);
  std::strftime(stamp.data(), stamp.size(), "%Y-%m-%dT%H-%M-%S", &local);
  return root / directories.data() / ("rollout-" + std::string(stamp.data()) + '-' + id + ".jsonl");
}

fs::path pi_path(const fs::path& root, const Session& target) {
  std::string directory = sanitize_cwd(target.cwd);
  if (!directory.starts_with("--")) directory = "--" + directory;
  if (!directory.ends_with("--")) directory += "--";
  std::string stamp = now_iso();
  std::ranges::replace(stamp, ':', '-');
  if (stamp.ends_with('Z')) stamp.pop_back();
  return root / directory / (stamp + '_' + target.id + ".jsonl");
}

struct Fingerprint {
  XXH128_hash_t hash{};
  bool operator==(const Fingerprint& other) const {
    return hash.low64 == other.hash.low64 && hash.high64 == other.hash.high64;
  }
};

struct FingerprintHash {
  std::size_t operator()(const Fingerprint& value) const noexcept {
    return static_cast<std::size_t>(value.hash.low64 ^ value.hash.high64);
  }
};

Fingerprint fingerprint(const Message& message) {
  const XXH64_hash_t seed = message.role == "assistant" ? 1 : 0;
  return {XXH3_128bits_withSeed(message.text.data(), message.text.size(), seed)};
}

}  // namespace

Session scan_session(const fs::path& path, Harness harness, const MessageFunction& on_message) {
  Session session;
  simdjson::dom::parser parser;
  each_jsonl_record(path, [&](const std::string& line) {
    simdjson::dom::element root_element;
    if (parser.parse(line).get(root_element) != simdjson::SUCCESS) return;
    simdjson::dom::object root;
    if (root_element.get_object().get(root) != simdjson::SUCCESS) return;
    const std::string type = string_field(root, "type");
    std::uint64_t ordinal = 0;
    if (root["ordinal"].get_uint64().get(ordinal) == simdjson::SUCCESS) {
      session.last_ordinal = std::max(session.last_ordinal, ordinal);
    }

    if (harness == Harness::claude) {
      if (session.id.empty()) session.id = string_field(root, "sessionId");
      if (session.cwd.empty()) session.cwd = string_field(root, "cwd");
      if (type == "ai-title") session.title = string_field(root, "aiTitle");
      if (type != "user" && type != "assistant") return;
      simdjson::dom::object message;
      if (root["message"].get_object().get(message) != simdjson::SUCCESS) return;
      simdjson::dom::element content;
      if (message["content"].get(content) != simdjson::SUCCESS) return;
      std::string text = content_text(content);
      if (text.empty()) return;
      session.last_uuid = string_field(root, "uuid");
      const std::string role = string_field(message, "role");
      if (role == "user" && session.first_user.empty()) session.first_user = text;
      if (on_message) on_message({role, std::move(text), string_field(root, "timestamp")});
      return;
    }

    if (harness == Harness::kiss || harness == Harness::pi || harness == Harness::openclaw) {
      if (type == "session") {
        session.id = string_field(root, "id");
        session.cwd = string_field(root, "cwd");
        return;
      }
      if (type == "session_info") {
        session.title = string_field(root, "name");
        return;
      }
      if (type != "message") return;
      simdjson::dom::object message;
      if (root["message"].get_object().get(message) != simdjson::SUCCESS) return;
      const std::string role = string_field(message, "role");
      if (role != "user" && role != "assistant") return;
      simdjson::dom::element content;
      if (message["content"].get(content) != simdjson::SUCCESS) return;
      std::string text = content_text(content);
      if (text.empty()) return;
      session.last_uuid = string_field(root, "id");
      if (role == "user" && session.first_user.empty()) session.first_user = text;
      if (on_message) on_message({role, std::move(text), string_field(root, "timestamp")});
      return;
    }

    simdjson::dom::object payload;
    if (root["payload"].get_object().get(payload) != simdjson::SUCCESS) return;
    if (type == "session_meta") {
      session.id = string_field(payload, "id");
      if (session.id.empty()) session.id = string_field(payload, "session_id");
      session.cwd = string_field(payload, "cwd");
      return;
    }
    if (type != "response_item") return;
    const std::string payload_type = string_field(payload, "type");
    if (payload_type == "message") {
      const std::string role = string_field(payload, "role");
      if (role != "user" && role != "assistant") return;
      simdjson::dom::element content;
      if (payload["content"].get(content) != simdjson::SUCCESS) return;
      std::string text = content_text(content);
      if (role == "user" && session.first_user.empty()) session.first_user = text;
      if (!text.empty() && on_message) {
        on_message({role, std::move(text), string_field(root, "timestamp")});
      }
    } else if (payload_type == "custom_tool_call" && on_message) {
      std::string text = "[Tool call: " + string_field(payload, "name") + "]";
      const std::string input = string_field(payload, "input");
      if (!input.empty()) text += "\n" + input;
      on_message({"assistant", std::move(text), string_field(root, "timestamp")});
    } else if (payload_type == "custom_tool_call_output" && on_message) {
      on_message({"user", "[Tool result]\n" + string_field(payload, "output"),
                  string_field(root, "timestamp")});
    }
  });
  if (harness == Harness::claude && session.id.empty()) session.id = path.stem().string();
  return session;
}

Session scan_session(const SessionRef& reference, const MessageFunction& on_message) {
  if (!is_database_harness(reference.harness)) {
    Session session = scan_session(reference.store, reference.harness, on_message);
    if (reference.harness == Harness::codex) {
      const fs::path index_path =
          reference.store.parent_path().parent_path().parent_path().parent_path().parent_path() /
          "session_index.jsonl";
      if (fs::exists(index_path)) {
        simdjson::dom::parser parser;
        each_jsonl_record(index_path, [&](const std::string& line) {
          simdjson::dom::object object;
          if (parser.parse(line).get_object().get(object) != simdjson::SUCCESS) return;
          if (string_field(object, "id") == session.id) {
            const std::string title = string_field(object, "thread_name");
            if (!title.empty()) session.title = title;
          }
        });
      }
    } else if (reference.harness == Harness::pi) {
      const fs::path index_path =
          reference.store.parent_path().parent_path() / "session-index.sqlite";
      if (fs::exists(index_path)) {
        Database database(index_path);
        Statement query =
            database.prepare("SELECT COALESCE(name, '') FROM sessions WHERE path = ?");
        query.bind(1, reference.store.string());
        if (query.row() && !query.text(0).empty()) session.title = query.text(0);
      }
    }
    return session;
  }

  Database database(reference.store);
  Session session;
  session.id = reference.id;
  if (reference.harness == Harness::hermes) {
    require_table(database, "sessions");
    require_table(database, "messages");
    Statement metadata = database.prepare(
        "SELECT COALESCE(cwd, ''), COALESCE(title, '') FROM sessions WHERE id = ?");
    metadata.bind(1, reference.id);
    if (!metadata.row()) throw std::runtime_error("Hermes session does not exist: " + reference.id);
    session.cwd = metadata.text(0);
    session.title = metadata.text(1);
    Statement messages = database.prepare(
        "SELECT role, COALESCE(content, '') FROM messages WHERE session_id = ? AND active = 1 "
        "AND role IN ('user', 'assistant') ORDER BY id");
    messages.bind(1, reference.id);
    while (messages.row()) {
      Message message{messages.text(0), messages.text(1), {}};
      if (message.text.empty()) continue;
      if (message.role == "user" && session.first_user.empty()) session.first_user = message.text;
      if (on_message) on_message(message);
    }
    return session;
  }

  if (reference.harness == Harness::opencode) {
    require_table(database, "session");
    require_table(database, "message");
    require_table(database, "part");
    Statement metadata = database.prepare(
        "SELECT directory, title FROM session WHERE id = ? AND time_archived IS NULL");
    metadata.bind(1, reference.id);
    if (!metadata.row())
      throw std::runtime_error("OpenCode session does not exist: " + reference.id);
    session.cwd = metadata.text(0);
    session.title = metadata.text(1);
    Statement messages = database.prepare(
        "SELECT m.id, json_extract(m.data, '$.role'), json_extract(p.data, '$.text') "
        "FROM message m JOIN part p ON p.message_id = m.id "
        "WHERE m.session_id = ? AND json_extract(m.data, '$.role') IN ('user', 'assistant') "
        "AND json_extract(p.data, '$.type') = 'text' ORDER BY m.time_created, m.id, p.id");
    messages.bind(1, reference.id);
    std::string message_id;
    Message current;
    const auto emit = [&] {
      if (current.text.empty()) return;
      if (current.role == "user" && session.first_user.empty()) session.first_user = current.text;
      if (on_message) on_message(current);
    };
    while (messages.row()) {
      if (!message_id.empty() && message_id != messages.text(0)) {
        emit();
        current = {};
      }
      message_id = messages.text(0);
      current.role = messages.text(1);
      if (!current.text.empty()) current.text += "\n\n";
      current.text += messages.text(2);
    }
    emit();
    return session;
  }

  require_table(database, "session_nodes");
  require_table(database, "session_windows");
  require_table(database, "transcript_events");
  Statement metadata = database.prepare(
      "SELECT COALESCE(w.display_name, n.display_name, n.label, '') "
      "FROM session_windows w JOIN session_nodes n ON n.session_key = w.session_key "
      "WHERE w.session_id = ?");
  metadata.bind(1, reference.id);
  if (!metadata.row()) throw std::runtime_error("OpenClaw session does not exist: " + reference.id);
  session.title = metadata.text(0);
  Statement events = database.prepare(
      "SELECT seq, event_json FROM transcript_events WHERE session_id = ? ORDER BY seq");
  events.bind(1, reference.id);
  simdjson::dom::parser parser;
  while (events.row()) {
    simdjson::dom::object root;
    session.last_ordinal = static_cast<std::uint64_t>(events.integer(0));
    if (parser.parse(events.text(1)).get_object().get(root) != simdjson::SUCCESS) continue;
    const std::string type = string_field(root, "type");
    if (type == "session") {
      session.cwd = string_field(root, "cwd");
      continue;
    }
    if (type != "message") continue;
    session.last_uuid = string_field(root, "id");
    simdjson::dom::object body;
    if (root["message"].get_object().get(body) != simdjson::SUCCESS) continue;
    const std::string role = string_field(body, "role");
    if (role != "user" && role != "assistant") continue;
    simdjson::dom::element content;
    if (body["content"].get(content) != simdjson::SUCCESS) continue;
    std::string text = content_text(content);
    if (text.empty()) continue;
    if (role == "user" && session.first_user.empty()) session.first_user = text;
    if (on_message) on_message({role, std::move(text), string_field(root, "timestamp")});
  }
  return session;
}

std::uint64_t session_stamp(const SessionRef& reference) {
  if (!is_database_harness(reference.harness)) return fs::file_size(reference.store);
  Database database(reference.store);
  const char* sql = reference.harness == Harness::hermes
                        ? "SELECT COALESCE(MAX(id), 0) FROM messages WHERE session_id = ?"
                    : reference.harness == Harness::opencode
                        ? "SELECT COUNT(*) FROM message WHERE session_id = ?"
                        : "SELECT COUNT(*) FROM transcript_events WHERE session_id = ?";
  Statement query = database.prepare(sql);
  query.bind(1, reference.id);
  return query.row() ? static_cast<std::uint64_t>(query.integer(0)) : 0;
}

std::vector<SessionRef> discover_sessions(const Store& store) {
  std::vector<SessionRef> sessions;
  if (!fs::exists(store.path)) return sessions;
  if (!is_database_harness(store.harness)) {
    std::error_code error;
    for (fs::recursive_directory_iterator
             iterator(store.path, fs::directory_options::skip_permission_denied, error),
         end;
         iterator != end; iterator.increment(error)) {
      if (error) {
        error.clear();
        continue;
      }
      if (!iterator->is_regular_file(error) || iterator->path().extension() != ".jsonl") continue;
      const std::string name = iterator->path().filename().string();
      if (store.harness == Harness::claude && name == "skill-injections.jsonl") continue;
      if (store.harness == Harness::codex && !name.starts_with("rollout-")) continue;
      Session session = scan_session(iterator->path(), store.harness);
      if (!session.id.empty()) {
        sessions.push_back({store.harness, iterator->path(), session.id,
                            static_cast<std::uint64_t>(iterator->file_size())});
      }
    }
    std::ranges::sort(sessions, {}, [](const SessionRef& session) { return session.store; });
    return sessions;
  }

  Database database(store.path);
  const char* sql = store.harness == Harness::hermes
                        ? "SELECT id FROM sessions WHERE archived = 0 AND hidden = 0"
                    : store.harness == Harness::opencode
                        ? "SELECT id FROM session WHERE time_archived IS NULL AND parent_id IS NULL"
                        : "SELECT session_id FROM session_windows WHERE ended_at IS NULL";
  Statement query = database.prepare(sql);
  while (query.row()) {
    SessionRef session{store.harness, store.path, query.text(0), 0};
    session.stamp = session_stamp(session);
    sessions.push_back(std::move(session));
  }
  std::ranges::sort(sessions, {}, [](const SessionRef& session) { return session.id; });
  return sessions;
}

std::string imported_title(Harness origin, const Session& source) {
  const std::string prefix = '[' + std::string(harness_name(origin)) + "]: ";
  std::string title = source.title.empty() ? source.first_user : source.title;
  const std::size_t newline = title.find('\n');
  if (newline != std::string::npos) title.resize(newline);
  if (title.size() > 80) title.resize(80);
  if (title.empty()) title = source.id;
  for (const Harness harness : {Harness::claude, Harness::codex, Harness::kiss, Harness::pi,
                                Harness::openclaw, Harness::hermes, Harness::opencode}) {
    const std::string old = '[' + std::string(harness_name(harness)) + "]: ";
    if (title.starts_with(old)) return title;
  }
  return prefix + title;
}

void append_claude(Appender& output, Session& target, const Message& message) {
  const std::string id = make_uuid();
  const std::string role = message.role == "assistant" ? "assistant" : "user";
  const std::string body = role == "assistant"
                               ? "{\"model\":\"janus-import\",\"id\":" + quote("msg_" + id) +
                                     ",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
                                     "\"type\":\"text\",\"text\":" +
                                     quote(message.text) +
                                     "}],\"stop_reason\":\"end_turn\",\"stop_sequence\":null}"
                               : "{\"role\":\"user\",\"content\":" + quote(message.text) + '}';
  output.line("{\"parentUuid\":" + (target.last_uuid.empty() ? "null" : quote(target.last_uuid)) +
              ",\"isSidechain\":false,\"userType\":\"external\",\"cwd\":" + quote(target.cwd) +
              ",\"sessionId\":" + quote(target.id) +
              ",\"version\":\"janus-0.0.1\",\"type\":" + quote(role) + ",\"uuid\":" + quote(id) +
              ",\"timestamp\":" + quote(message.timestamp.empty() ? now_iso() : message.timestamp) +
              ",\"message\":" + body + '}');
  target.last_uuid = id;
}

void append_codex(Appender& output, Session& target, const Message& message) {
  const std::string role = message.role == "assistant" ? "assistant" : "user";
  const std::string kind = role == "assistant" ? "output_text" : "input_text";
  output.line(
      "{\"timestamp\":" + quote(message.timestamp.empty() ? now_iso() : message.timestamp) +
      ",\"ordinal\":" + std::to_string(++target.last_ordinal) +
      ",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":" + quote(role) +
      ",\"content\":[{\"type\":" + quote(kind) + ",\"text\":" + quote(message.text) + "}]}}");
}

void append_pi(Appender& output, Session& target, const Message& message) {
  const std::string id = make_uuid();
  const std::string role = message.role == "assistant" ? "assistant" : "user";
  const std::string body =
      role == "assistant"
          ? "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":" +
                quote(message.text) +
                "}],\"provider\":\"janus\",\"model\":\"import\","
                "\"stopReason\":\"stop\",\"timestamp\":" +
                quote(message.timestamp.empty() ? now_iso() : message.timestamp) + '}'
          : "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":" + quote(message.text) +
                "}],\"timestamp\":" +
                quote(message.timestamp.empty() ? now_iso() : message.timestamp) + '}';
  output.line("{\"type\":\"message\",\"id\":" + quote(id) +
              ",\"parentId\":" + (target.last_uuid.empty() ? "null" : quote(target.last_uuid)) +
              ",\"timestamp\":" + quote(message.timestamp.empty() ? now_iso() : message.timestamp) +
              ",\"message\":" + body + '}');
  target.last_uuid = id;
}

SessionRef create_peer(const Store& destination, const SessionRef& source_reference) {
  Session source = scan_session(source_reference);
  if (source.id.empty())
    throw std::runtime_error("session metadata is missing in " + source_reference.store.string());
  source.title = imported_title(source_reference.harness, source);
  Session target{make_uuid(),  source.cwd.empty() ? fs::current_path().string() : source.cwd,
                 {},           0,
                 source.title, {}};
  SessionRef reference{destination.harness, destination.path, target.id, 0};

  if (!is_database_harness(destination.harness)) {
    reference.store = destination.harness == Harness::codex
                          ? codex_path(destination.path, target.id)
                      : destination.harness == Harness::claude
                          ? destination.path / sanitize_cwd(target.cwd) / (target.id + ".jsonl")
                          : pi_path(destination.path, target);
    fs::create_directories(reference.store.parent_path());
    const fs::path temporary = reference.store.string() + ".tmp";
    {
      Appender output(temporary);
      if (destination.harness == Harness::codex) {
        const std::string timestamp = now_iso();
        output.line("{\"timestamp\":" + quote(timestamp) +
                    ",\"ordinal\":0,\"type\":\"session_meta\",\"payload\":{\"session_id\":" +
                    quote(target.id) + ",\"id\":" + quote(target.id) +
                    ",\"timestamp\":" + quote(timestamp) + ",\"cwd\":" + quote(target.cwd) +
                    ",\"originator\":\"janus\",\"cli_version\":\"0.0.0\",\"source\":\"cli\","
                    "\"model_provider\":\"openai\",\"history_mode\":\"legacy\"}}");
      } else if (destination.harness == Harness::kiss || destination.harness == Harness::pi) {
        const std::string header_id = make_uuid();
        output.line("{\"type\":\"session\",\"version\":3,\"id\":" + quote(target.id) +
                    ",\"timestamp\":" + quote(now_iso()) + ",\"cwd\":" + quote(target.cwd) + '}');
        output.line("{\"type\":\"session_info\",\"id\":" + quote(header_id) +
                    ",\"parentId\":null,\"timestamp\":" + quote(now_iso()) +
                    ",\"name\":" + quote(target.title) + '}');
        target.last_uuid = header_id;
      } else if (destination.harness == Harness::claude) {
        output.line("{\"type\":\"ai-title\",\"sessionId\":" + quote(target.id) +
                    ",\"aiTitle\":" + quote(target.title) + '}');
      } else {
        throw std::runtime_error("target is not a JSONL harness");
      }
      scan_session(source_reference, [&](const Message& message) {
        if (destination.harness == Harness::codex)
          append_codex(output, target, message);
        else if (destination.harness == Harness::claude)
          append_claude(output, target, message);
        else
          append_pi(output, target, message);
      });
    }
    fs::rename(temporary, reference.store);
    if (destination.harness == Harness::codex) {
      Appender index(destination.path.parent_path() / "session_index.jsonl");
      index.line("{\"id\":" + quote(target.id) + ",\"thread_name\":" + quote(target.title) +
                 ",\"updated_at\":" + quote(now_iso()) + '}');
    }
    reference.stamp = session_stamp(reference);
    return reference;
  }

  Database database(destination.path);
  Transaction transaction(database);
  const std::int64_t now = now_milliseconds();
  if (destination.harness == Harness::hermes) {
    require_table(database, "sessions");
    require_table(database, "messages");
    Statement duplicate = database.prepare("SELECT 1 FROM sessions WHERE title = ? LIMIT 1");
    duplicate.bind(1, target.title);
    if (duplicate.row()) target.title += " (" + target.id.substr(0, 8) + ')';
    Statement insert = database.prepare(
        "INSERT INTO sessions (id, source, started_at, message_count, tool_call_count, cwd, title, "
        "title_source, last_activity_at) VALUES (?, 'cli', ?, 0, 0, ?, ?, 'imported', ?)");
    insert.bind(1, target.id);
    insert.bind(2, now / 1000);
    insert.bind(3, target.cwd);
    insert.bind(4, target.title);
    insert.bind(5, now / 1000);
    insert.row();
  } else if (destination.harness == Harness::opencode) {
    require_table(database, "session");
    require_table(database, "message");
    require_table(database, "part");
    require_table(database, "project");
    std::string project_id;
    Statement project = database.prepare(
        "SELECT id FROM project WHERE worktree = ? OR ? LIKE worktree || '/%' "
        "ORDER BY length(worktree) DESC LIMIT 1");
    project.bind(1, target.cwd);
    project.bind(2, target.cwd);
    if (project.row()) project_id = project.text(0);
    if (project_id.empty()) {
      Statement fallback =
          database.prepare("SELECT id FROM project ORDER BY time_updated DESC LIMIT 1");
      if (fallback.row()) project_id = fallback.text(0);
    }
    if (project_id.empty()) throw std::runtime_error("OpenCode database has no project row");
    target.id = "ses_" + target.id;
    reference.id = target.id;
    Statement insert = database.prepare(
        "INSERT INTO session (id, project_id, slug, directory, title, version, time_created, "
        "time_updated) VALUES (?, ?, ?, ?, ?, 'janus', ?, ?)");
    insert.bind(1, target.id);
    insert.bind(2, project_id);
    insert.bind(3, target.id);
    insert.bind(4, target.cwd);
    insert.bind(5, target.title);
    insert.bind(6, now);
    insert.bind(7, now);
    insert.row();
  } else {
    require_table(database, "session_nodes");
    require_table(database, "session_windows");
    require_table(database, "transcript_events");
    require_table(database, "transcript_rewrite_watermarks");
    const std::string key = "agent:main:explicit:janus-" + target.id;
    const std::string entry = "{\"sessionId\":" + quote(target.id) +
                              ",\"updatedAt\":" + std::to_string(now) +
                              ",\"displayName\":" + quote(target.title) + '}';
    Statement node = database.prepare(
        "INSERT INTO session_nodes (session_key, current_session_id, entry_json, entry_valid, "
        "updated_at, created_at, created_via, created_actor_type, display_name, label) "
        "VALUES (?, ?, ?, -1, ?, ?, 'operator', 'human', ?, ?)");
    node.bind(1, key);
    node.bind(2, target.id);
    node.bind(3, entry);
    node.bind(4, now);
    node.bind(5, now);
    node.bind(6, target.title);
    node.bind(7, target.title);
    node.row();
    Statement valid =
        database.prepare("UPDATE session_nodes SET entry_valid = -1 WHERE session_key = ?");
    valid.bind(1, key);
    valid.row();
    Statement window = database.prepare(
        "INSERT INTO session_windows (session_id, session_key, session_scope, created_at, "
        "updated_at, started_at, display_name) VALUES (?, ?, 'conversation', ?, ?, ?, ?)");
    window.bind(1, target.id);
    window.bind(2, key);
    window.bind(3, now);
    window.bind(4, now);
    window.bind(5, now);
    window.bind(6, target.title);
    window.row();
    Statement watermark = database.prepare(
        "INSERT INTO transcript_rewrite_watermarks (session_id, generation, updated_at) VALUES (?, "
        "?, ?)");
    watermark.bind(1, target.id);
    watermark.bind(2, make_uuid());
    watermark.bind(3, now);
    watermark.row();
    const std::string header = "{\"type\":\"session\",\"version\":3,\"id\":" + quote(target.id) +
                               ",\"timestamp\":" + quote(now_iso()) +
                               ",\"cwd\":" + quote(target.cwd) + '}';
    Statement event = database.prepare(
        "INSERT INTO transcript_events (session_id, seq, event_json, created_at) VALUES (?, 0, ?, "
        "?)");
    event.bind(1, target.id);
    event.bind(2, header);
    event.bind(3, now);
    event.row();
  }

  std::size_t count = 0;
  scan_session(source_reference, [&](const Message& message) {
    append_database_message(database, reference, target, message);
    ++count;
  });
  finish_database_append(database, reference, count);
  transaction.commit();
  reference.stamp = session_stamp(reference);
  return reference;
}

fs::path create_file_peer(const fs::path& root, const fs::path& source_path, Harness source_harness,
                          Harness target_harness) {
  const Session source = scan_session(source_path, source_harness);
  return create_peer(Store{target_harness, root},
                     SessionRef{source_harness, source_path, source.id, fs::file_size(source_path)})
      .store;
}

fs::path create_peer(const fs::path& root, const fs::path& source_path, Harness source_harness) {
  const Harness target_harness =
      source_harness == Harness::claude ? Harness::codex : Harness::claude;
  return create_file_peer(root, source_path, source_harness, target_harness);
}

std::size_t copy_missing(const fs::path& source_path, Harness source_harness,
                         const fs::path& target_path, Harness target_harness) {
  std::unordered_map<Fingerprint, std::uint32_t, FingerprintHash> remaining;
  Session target = scan_session(target_path, target_harness,
                                [&](const Message& message) { ++remaining[fingerprint(message)]; });
  Appender output(target_path);
  std::size_t copied = 0;
  scan_session(source_path, source_harness, [&](const Message& message) {
    auto found = remaining.find(fingerprint(message));
    if (found != remaining.end() && found->second > 0) {
      if (--found->second == 0) remaining.erase(found);
      return;
    }
    if (target_harness == Harness::codex)
      append_codex(output, target, message);
    else if (target_harness == Harness::claude)
      append_claude(output, target, message);
    else
      append_pi(output, target, message);
    ++copied;
  });
  return copied;
}

std::size_t copy_missing(const SessionRef& source, const SessionRef& target_reference) {
  if (!is_database_harness(source.harness) && !is_database_harness(target_reference.harness)) {
    return copy_missing(source.store, source.harness, target_reference.store,
                        target_reference.harness);
  }

  std::unordered_map<Fingerprint, std::uint32_t, FingerprintHash> remaining;
  Session target = scan_session(target_reference,
                                [&](const Message& message) { ++remaining[fingerprint(message)]; });
  std::size_t copied = 0;
  if (!is_database_harness(target_reference.harness)) {
    Appender output(target_reference.store);
    scan_session(source, [&](const Message& message) {
      auto found = remaining.find(fingerprint(message));
      if (found != remaining.end() && found->second > 0) {
        if (--found->second == 0) remaining.erase(found);
        return;
      }
      if (target_reference.harness == Harness::codex)
        append_codex(output, target, message);
      else if (target_reference.harness == Harness::claude)
        append_claude(output, target, message);
      else
        append_pi(output, target, message);
      ++copied;
    });
    return copied;
  }

  Database database(target_reference.store);
  Transaction transaction(database);
  scan_session(source, [&](const Message& message) {
    auto found = remaining.find(fingerprint(message));
    if (found != remaining.end() && found->second > 0) {
      if (--found->second == 0) remaining.erase(found);
      return;
    }
    append_database_message(database, target_reference, target, message);
    ++copied;
  });
  finish_database_append(database, target_reference, copied);
  transaction.commit();
  return copied;
}

}  // namespace janus
