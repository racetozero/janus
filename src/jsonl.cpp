#include "janus/jsonl.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>

namespace janus {

std::string quote(std::string_view text) {
  std::string output;
  output.reserve(text.size() + 2);
  output.push_back('"');
  for (const unsigned char character : text) {
    switch (character) {
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (character < 0x20) {
          std::array<char, 7> escaped{};
          std::snprintf(escaped.data(), escaped.size(), "\\u%04x", character);
          output += escaped.data();
        } else {
          output.push_back(static_cast<char>(character));
        }
    }
  }
  output.push_back('"');
  return output;
}

std::string now_iso() {
  const std::time_t seconds =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  std::array<char, 21> output{};
  std::strftime(output.data(), output.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return output.data();
}

std::string make_uuid() {
  static thread_local std::mt19937_64 random{std::random_device{}()};
  const std::uint64_t high = (random() & 0xffffffffffff0fffULL) | 0x4000ULL;
  const std::uint64_t low = (random() & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;
  std::array<char, 37> output{};
  std::snprintf(output.data(), output.size(), "%08x-%04x-%04x-%04x-%012llx",
                static_cast<unsigned>(high >> 32), static_cast<unsigned>((high >> 16) & 0xffff),
                static_cast<unsigned>(high & 0xffff), static_cast<unsigned>(low >> 48),
                static_cast<unsigned long long>(low & 0xffffffffffffULL));
  return output.data();
}

fs::path user_home() {
  const char* value = std::getenv("HOME");
  if (value == nullptr || *value == '\0') throw std::runtime_error("HOME is not set");
  return value;
}

Appender::Appender(const fs::path& path) : output_(path, std::ios::binary | std::ios::app) {
  if (!output_) throw std::runtime_error("cannot write " + path.string());
}

void Appender::line(std::string_view value) {
  output_.write(value.data(), static_cast<std::streamsize>(value.size()));
  output_.put('\n');
  if (!output_) throw std::runtime_error("session write failed");
}

}  // namespace janus
