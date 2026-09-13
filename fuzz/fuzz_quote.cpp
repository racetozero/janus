#include <cstddef>
#include <cstdint>
#include <string_view>

#include "janus/jsonl.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static_cast<void>(janus::quote(std::string_view(reinterpret_cast<const char*>(data), size)));
  return 0;
}
