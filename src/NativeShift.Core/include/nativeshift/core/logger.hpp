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
                    bool include_paths_in_debug = false);

    void Log(LogLevel level, std::string_view event, std::string_view message);
    void LogConversion(const ConversionResult& result);

    [[nodiscard]] const std::filesystem::path& Path() const noexcept;
    [[nodiscard]] static std::filesystem::path DefaultLogPath();

  private:
    std::filesystem::path path_;
    bool include_paths_in_debug_{};
    std::mutex mutex_;
};

} // namespace nativeshift::core
