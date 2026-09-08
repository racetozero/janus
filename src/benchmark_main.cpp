#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "janus/benchmark.hpp"

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--suite") {
      bool header = true;
      for (const std::size_t count : {1000U, 10000U, 50000U}) {
        janus::benchmark(count, header);
        header = false;
      }
      return 0;
    }
    if (argc != 2) throw std::runtime_error("usage: janus-benchmark MESSAGES|--suite");
    std::size_t used = 0;
    const std::size_t count = std::stoull(argv[1], &used);
    if (count == 0 || used != std::string_view(argv[1]).size()) {
      throw std::runtime_error("MESSAGES must be a positive integer");
    }
    janus::benchmark(count);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "janus-benchmark: " << error.what() << '\n';
    return 1;
  }
}
