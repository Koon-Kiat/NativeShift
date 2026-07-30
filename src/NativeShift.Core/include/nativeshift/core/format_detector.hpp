#pragma once

#include "nativeshift/core/formats.hpp"

#include <filesystem>
#include <string>

namespace nativeshift::core {

struct FileProbeResult {
    FileFormat format{FileFormat::Unknown};
    std::string message;
};

[[nodiscard]] FileProbeResult
DetectFormat(const std::filesystem::path& input_path);

} // namespace nativeshift::core
