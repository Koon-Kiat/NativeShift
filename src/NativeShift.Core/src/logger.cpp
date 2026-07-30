#include "nativeshift/core/logger.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

namespace nativeshift::core {
namespace {

std::string_view ToString(const LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Information:
        return "information";
    case LogLevel::Warning:
        return "warning";
    case LogLevel::Error:
    default:
        return "error";
    }
}

std::string UtcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

} // namespace

Logger::Logger(std::filesystem::path log_path,
               const bool include_paths_in_debug)
    : path_(std::move(log_path)),
      include_paths_in_debug_(include_paths_in_debug) {}

void Logger::Log(const LogLevel level, const std::string_view event,
                 const std::string_view message) {
    std::scoped_lock lock(mutex_);
    std::error_code error;
    std::filesystem::create_directories(path_.parent_path(), error);
    if (error) {
        return;
    }
    std::ofstream output(path_, std::ios::app);
    if (!output) {
        return;
    }
    const nlohmann::json entry{
        {"timestamp", UtcTimestamp()},
        {"level", ToString(level)},
        {"event", event},
        {"message", message},
        {"application_version", NATIVESHIFT_VERSION},
    };
    output << entry.dump() << '\n';
}

void Logger::LogConversion(const ConversionResult& result) {
    std::scoped_lock lock(mutex_);
    std::error_code error;
    std::filesystem::create_directories(path_.parent_path(), error);
    if (error) {
        return;
    }
    std::ofstream output(path_, std::ios::app);
    if (!output) {
        return;
    }
    nlohmann::json entry{
        {"timestamp", UtcTimestamp()},
        {"level",
         result.status == ConversionStatus::Failed ? "error" : "information"},
        {"event", "conversion_completed"},
        {"application_version", NATIVESHIFT_VERSION},
        {"provider", result.provider},
        {"input_format", ToString(result.input_format)},
        {"output_format", ToString(result.output_format)},
        {"duration_ms", result.duration.count()},
        {"status", ToString(result.status)},
        {"failure_category", ToString(result.error)},
        {"hardware_acceleration", "not_applicable"},
    };
    if (include_paths_in_debug_) {
        entry["output_path"] = PathToUtf8(result.output_path);
    }
    output << entry.dump() << '\n';
}

const std::filesystem::path& Logger::Path() const noexcept { return path_; }

std::filesystem::path Logger::DefaultLogPath() {
#ifdef _WIN32
    wchar_t* local_app_data = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&local_app_data, &length, L"LOCALAPPDATA") == 0 &&
        local_app_data != nullptr) {
        const auto result = std::filesystem::path(local_app_data) /
                            "NativeShift" / "Logs" / "converter.jsonl";
        std::free(local_app_data);
        return result;
    }
#endif
    return std::filesystem::temp_directory_path() / "NativeShift" / "Logs" /
           "converter.jsonl";
}

} // namespace nativeshift::core
