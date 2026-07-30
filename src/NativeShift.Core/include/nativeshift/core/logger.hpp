#pragma once

#include "nativeshift/core/models.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace nativeshift::core {

enum class LogLevel { Debug, Information, Warning, Error };

class Logger {
  public:
    explicit Logger(std::filesystem::path log_path = DefaultLogPath(),
                    bool include_paths_in_debug = false,
                    LogLevel minimum_level = LogLevel::Information,
                    std::uintmax_t maximum_file_bytes = 5U * 1024U * 1024U,
                    std::size_t retained_files = 3);

    void Log(LogLevel level, std::string_view event, std::string_view message);
    void LogConversion(const ConversionResult& result);

    [[nodiscard]] const std::filesystem::path& Path() const noexcept;
    [[nodiscard]] std::string
    DiagnosticSummary(std::string_view media_library_version = {}) const;
    [[nodiscard]] static std::filesystem::path DefaultLogPath();

  private:
    void RotateIfNeeded();

    std::filesystem::path path_;
    bool include_paths_in_debug_{};
    LogLevel minimum_level_{LogLevel::Information};
    std::uintmax_t maximum_file_bytes_{};
    std::size_t retained_files_{};
    std::mutex mutex_;
};

} // namespace nativeshift::core
