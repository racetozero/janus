#pragma once

#include <optional>
#include <string_view>

namespace janus {

void update();

// The result is negative, zero, or positive, and has no value if a version is not numeric.
std::optional<int> compare_versions(std::string_view left, std::string_view right);

}  // namespace janus
