#pragma once

#include <array>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "janus/types.hpp"

namespace janus {

std::string quote(std::string_view text);
std::string now_iso();
std::string make_uuid();
fs::path user_home();

class Appender {
 public:
  explicit Appender(const fs::path& path);
  void line(std::string_view value);

 private:
  std::ofstream output_;
};

template <class Function>
void each_jsonl_record(const fs::path& path, Function&& function) {
  std::ifstream input(path, std::ios::binary);
  std::error_code size_error;
  const std::uintmax_t expected = fs::file_size(path, size_error);
  if (!input || size_error) throw std::runtime_error("cannot read " + path.string());

  std::array<char, 64U * 1024U> buffer{};
  std::string line;
  line.reserve(buffer.size());
  bool overflow = false;
  std::uintmax_t consumed = 0;

  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = static_cast<std::size_t>(input.gcount());
    consumed += count;
    std::size_t begin = 0;
    for (std::size_t index = 0; index < count; ++index) {
      if (buffer[index] != '\n') continue;
      if (!overflow && line.size() + index - begin <= max_record_bytes) {
        line.append(buffer.data() + begin, index - begin);
        if (!line.empty()) function(line);
      }
      line.clear();
      overflow = false;
      begin = index + 1;
    }
    if (begin >= count || overflow) continue;
    const std::size_t tail = count - begin;
    if (line.size() + tail <= max_record_bytes) {
      line.append(buffer.data() + begin, tail);
    } else {
      line.clear();
      overflow = true;
    }
  }
  if (consumed < expected) {
    throw std::runtime_error("cannot finish reading " + path.string() + "; read " +
                             std::to_string(consumed) + " of " + std::to_string(expected) +
                             " bytes; Janus tries again on the next sync");
  }
  // A final record with no newline can be complete. Parsers skip it if it is partial.
  if (!overflow && !line.empty()) function(line);
}

}  // namespace janus
