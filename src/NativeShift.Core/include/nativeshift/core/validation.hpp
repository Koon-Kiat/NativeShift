#pragma once

#include "nativeshift/core/models.hpp"

#include <vector>

namespace nativeshift::core {

inline constexpr std::uint32_t kMaximumImageDimension = 32'768;
inline constexpr std::uint64_t kMaximumImagePixels = 100'000'000;
inline constexpr std::uintmax_t kMaximumEncodedImageBytes =
    512ULL * 1024ULL * 1024ULL;

[[nodiscard]] std::vector<ValidationIssue>
ValidateRequestBasics(const ConversionRequest& request);

} // namespace nativeshift::core
