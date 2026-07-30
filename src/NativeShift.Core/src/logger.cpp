#include "nativeshift/core/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

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
               const bool include_paths_in_debug, const LogLevel minimum_level,
               const std::uintmax_t maximum_file_bytes,
               const std::size_t retained_files)
    : path_(std::move(log_path)),
      include_paths_in_debug_(include_paths_in_debug),
      minimum_level_(minimum_level),
      maximum_file_bytes_(std::max<std::uintmax_t>(1024, maximum_file_bytes)),
      retained_files_(std::clamp<std::size_t>(retained_files, 1, 10)) {}

void Logger::Log(const LogLevel level, const std::string_view event,
                 const std::string_view message) {
    if (level < minimum_level_) {
        return;
    }
    std::scoped_lock lock(mutex_);
    std::error_code error;
    std::filesystem::create_directories(path_.parent_path(), error);
    if (error) {
        return;
    }
    RotateIfNeeded();
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
    const auto level = result.status == ConversionStatus::Failed
                           ? LogLevel::Error
                       : result.warnings.empty() ? LogLevel::Information
                                                 : LogLevel::Warning;
    if (level < minimum_level_) {
        return;
    }
    std::scoped_lock lock(mutex_);
    std::error_code error;
    std::filesystem::create_directories(path_.parent_path(), error);
    if (error) {
        return;
    }
    RotateIfNeeded();
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
        {"job_id", result.job_id},
        {"provider", result.provider},
        {"input_format", ToString(result.input_format)},
        {"output_format", ToString(result.output_format)},
        {"duration_ms", result.duration.count()},
        {"status", ToString(result.status)},
        {"failure_category", ToString(result.error)},
        {"selected_codec", result.selected_codec},
        {"hardware_acceleration", result.hardware_acceleration},
        {"warning_count", result.warnings.size()},
    };
    if (include_paths_in_debug_) {
        entry["output_path"] = PathToUtf8(result.output_path);
    }
    output << entry.dump() << '\n';
}

const std::filesystem::path& Logger::Path() const noexcept { return path_; }

std::string
Logger::DiagnosticSummary(const std::string_view media_library_version) const {
    nlohmann::json summary{
        {"application", "NativeShift"},
        {"application_version", NATIVESHIFT_VERSION},
        {"logical_processors", std::thread::hardware_concurrency()},
        {"media_library_version", media_library_version},
#ifdef _WIN32
        {"platform", "Windows"},
#else
        {"platform", "Unknown"},
#endif
        {"paths_included", false},
    };
    return summary.dump(2);
}

void Logger::RotateIfNeeded() {
    std::error_code error;
    if (!std::filesystem::exists(path_, error) || error ||
        std::filesystem::file_size(path_, error) < maximum_file_bytes_ ||
        error) {
        return;
    }

    const auto rotated_path = [this](const std::size_t index) {
        auto path = path_;
        path += L"." + std::to_wstring(index);
        return path;
    };
    std::filesystem::remove(rotated_path(retained_files_), error);
    for (auto index = retained_files_; index > 1; --index) {
        error.clear();
        const auto source = rotated_path(index - 1);
        if (std::filesystem::exists(source, error) && !error) {
            std::filesystem::rename(source, rotated_path(index), error);
        }
    }
    error.clear();
    std::filesystem::rename(path_, rotated_path(1), error);
}

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
