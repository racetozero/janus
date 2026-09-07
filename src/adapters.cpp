#include "janus/adapters.hpp"

#include <simdjson.h>
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <unordered_map>

namespace janus {
namespace {

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
      if (type != "user" && type != "assistant") return;
      simdjson::dom::object message;
      if (root["message"].get_object().get(message) != simdjson::SUCCESS) return;
      simdjson::dom::element content;
      if (message["content"].get(content) != simdjson::SUCCESS) return;
      std::string text = content_text(content);
      if (text.empty()) return;
      session.last_uuid = string_field(root, "uuid");
      if (on_message) {
        on_message(
            {string_field(message, "role"), std::move(text), string_field(root, "timestamp")});
      }
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

fs::path create_peer(const fs::path& root, const fs::path& source_path, Harness source_harness) {
  const Session source = scan_session(source_path, source_harness);
  if (source.id.empty())
    throw std::runtime_error("session metadata is missing in " + source_path.string());
  Session target{make_uuid(), source.cwd.empty() ? fs::current_path().string() : source.cwd, {}, 0};
  const Harness target_harness =
      source_harness == Harness::claude ? Harness::codex : Harness::claude;
  const fs::path path = target_harness == Harness::codex
                            ? codex_path(root, target.id)
                            : root / sanitize_cwd(target.cwd) / (target.id + ".jsonl");
  fs::create_directories(path.parent_path());
  const fs::path temporary = path.string() + ".tmp";
  {
    Appender output(temporary);
    if (target_harness == Harness::codex) {
      const std::string timestamp = now_iso();
      output.line("{\"timestamp\":" + quote(timestamp) +
                  ",\"ordinal\":0,\"type\":\"session_meta\",\"payload\":{\"session_id\":" +
                  quote(target.id) + ",\"id\":" + quote(target.id) +
                  ",\"timestamp\":" + quote(timestamp) + ",\"cwd\":" + quote(target.cwd) +
                  ",\"originator\":\"janus\",\"cli_version\":\"0.0.0\",\"source\":\"cli\","
                  "\"model_provider\":\"openai\",\"history_mode\":\"legacy\"}}");
    }
    scan_session(source_path, source_harness, [&](const Message& message) {
      if (target_harness == Harness::codex)
        append_codex(output, target, message);
      else
        append_claude(output, target, message);
    });
  }
  fs::rename(temporary, path);
  return path;
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
    else
      append_claude(output, target, message);
    ++copied;
  });
  return copied;
}

}  // namespace janus
