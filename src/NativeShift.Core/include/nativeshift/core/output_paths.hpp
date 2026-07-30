#pragma once

#include "nativeshift/core/models.hpp"

#include <filesystem>
#include <string>

namespace nativeshift::core {

struct OutputResolution {
    std::filesystem::path path;
    bool skip{false};
    std::string error;
};

[[nodiscard]] std::filesystem::path
BuildOutputPath(const std::filesystem::path& input,
                const std::filesystem::path& output_directory,
                FileFormat output_format);
[[nodiscard]] std::filesystem::path
SanitizeFilenameStem(const std::filesystem::path& stem);
[[nodiscard]] bool
IsSafeOutputFilename(const std::filesystem::path& filename) noexcept;
[[nodiscard]] std::filesystem::path
GenerateUniqueOutputPath(const std::filesystem::path& desired);
[[nodiscard]] OutputResolution
ResolveOutputConflict(const std::filesystem::path& desired,
                      OutputConflictPolicy policy);
[[nodiscard]] std::filesystem::path
MakeTemporaryOutputPath(const std::filesystem::path& final_path);
[[nodiscard]] bool
CommitTemporaryOutput(const std::filesystem::path& temporary_path,
                      const std::filesystem::path& final_path, bool replace,
                      std::string& error);

} // namespace nativeshift::core
