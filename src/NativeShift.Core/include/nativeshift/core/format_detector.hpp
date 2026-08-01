#pragma once

#include "nativeshift/core/formats.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace nativeshift::core {

struct FileProbeResult {
    FileFormat format{FileFormat::Unknown};
    std::string message;
};

[[nodiscard]] FileProbeResult
DetectFormat(const std::filesystem::path& input_path);
[[nodiscard]] FileProbeResult DetectFormat(std::span<const std::uint8_t> bytes);

} // namespace nativeshift::core
