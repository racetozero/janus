#pragma once

#include <cstddef>

namespace janus {

void self_test();
void benchmark(std::size_t message_count, bool header = true);

}  // namespace janus
