#include "nativeshift/core/formats.hpp"
#include "nativeshift/core/models.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace nativeshift::core {
namespace {

std::string Lowercase(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

} // namespace

std::string_view ToString(const FileFormat format) noexcept {
    switch (format) {
    case FileFormat::Png:
        return "png";
    case FileFormat::Jpeg:
        return "jpeg";
    case FileFormat::WebP:
        return "webp";
    case FileFormat::Bmp:
        return "bmp";
    case FileFormat::Tiff:
        return "tiff";
    case FileFormat::Mp3:
        return "mp3";
    case FileFormat::Wav:
        return "wav";
    case FileFormat::Flac:
        return "flac";
    case FileFormat::Aac:
        return "aac";
    case FileFormat::M4a:
        return "m4a";
    case FileFormat::Ogg:
        return "ogg";
    case FileFormat::Mp4:
        return "mp4";
    case FileFormat::Mkv:
        return "mkv";
    case FileFormat::WebM:
        return "webm";
    case FileFormat::Mov:
        return "mov";
    case FileFormat::Avi:
        return "avi";
    case FileFormat::Unknown:
    default:
        return "unknown";
    }
}

std::optional<FileFormat> FormatFromString(const std::string_view value) {
    const auto normalized = Lowercase(value);
    static const std::unordered_map<std::string, FileFormat> formats{
        {"png", FileFormat::Png},   {"jpg", FileFormat::Jpeg},
        {"jpeg", FileFormat::Jpeg}, {"webp", FileFormat::WebP},
        {"bmp", FileFormat::Bmp},   {"tif", FileFormat::Tiff},
        {"tiff", FileFormat::Tiff}, {"mp3", FileFormat::Mp3},
        {"wav", FileFormat::Wav},   {"flac", FileFormat::Flac},
        {"aac", FileFormat::Aac},   {"m4a", FileFormat::M4a},
        {"ogg", FileFormat::Ogg},   {"mp4", FileFormat::Mp4},
        {"mkv", FileFormat::Mkv},   {"webm", FileFormat::WebM},
        {"mov", FileFormat::Mov},   {"avi", FileFormat::Avi},
    };
    const auto found = formats.find(normalized);
    if (found == formats.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::string_view ExtensionFor(const FileFormat format) noexcept {
    if (format == FileFormat::Jpeg) {
        return ".jpg";
    }
    if (format == FileFormat::Unknown) {
        return {};
    }
    switch (format) {
    case FileFormat::Png:
        return ".png";
    case FileFormat::WebP:
        return ".webp";
    case FileFormat::Bmp:
        return ".bmp";
    case FileFormat::Tiff:
        return ".tiff";
    case FileFormat::Mp3:
        return ".mp3";
    case FileFormat::Wav:
        return ".wav";
    case FileFormat::Flac:
        return ".flac";
    case FileFormat::Aac:
        return ".aac";
    case FileFormat::M4a:
        return ".m4a";
    case FileFormat::Ogg:
        return ".ogg";
    case FileFormat::Mp4:
        return ".mp4";
    case FileFormat::Mkv:
        return ".mkv";
    case FileFormat::WebM:
        return ".webm";
    case FileFormat::Mov:
        return ".mov";
    case FileFormat::Avi:
        return ".avi";
    case FileFormat::Jpeg:
        return ".jpg";
    case FileFormat::Unknown:
    default:
        return {};
    }
}

MediaKind KindOf(const FileFormat format) noexcept {
    switch (format) {
    case FileFormat::Png:
    case FileFormat::Jpeg:
    case FileFormat::WebP:
    case FileFormat::Bmp:
    case FileFormat::Tiff:
        return MediaKind::Image;
    case FileFormat::Mp3:
    case FileFormat::Wav:
    case FileFormat::Flac:
    case FileFormat::Aac:
    case FileFormat::M4a:
    case FileFormat::Ogg:
        return MediaKind::Audio;
    case FileFormat::Mp4:
    case FileFormat::Mkv:
    case FileFormat::WebM:
    case FileFormat::Mov:
    case FileFormat::Avi:
        return MediaKind::Video;
    case FileFormat::Unknown:
    default:
        return MediaKind::Unknown;
    }
}

bool IsImageFormat(const FileFormat format) noexcept {
    return KindOf(format) == MediaKind::Image;
}

ProviderOutcome ProviderOutcome::Succeeded() {
    ProviderOutcome outcome;
    outcome.success = true;
    return outcome;
}

ProviderOutcome ProviderOutcome::Failed(const ErrorCategory category,
                                        std::string message) {
    ProviderOutcome outcome;
    outcome.error = category;
    outcome.message = std::move(message);
    return outcome;
}

ProviderOutcome ProviderOutcome::Cancelled(std::string message) {
    ProviderOutcome outcome;
    outcome.cancelled = true;
    outcome.error = ErrorCategory::Cancelled;
    outcome.message = std::move(message);
    return outcome;
}

std::string_view ToString(const OutputConflictPolicy policy) noexcept {
    switch (policy) {
    case OutputConflictPolicy::Ask:
        return "ask";
    case OutputConflictPolicy::Skip:
        return "skip";
    case OutputConflictPolicy::Replace:
        return "replace";
    case OutputConflictPolicy::GenerateUniqueName:
        return "unique";
    default:
        return "ask";
    }
}

std::optional<OutputConflictPolicy>
OutputConflictPolicyFromString(const std::string_view value) {
    const auto normalized = Lowercase(value);
    if (normalized == "ask") {
        return OutputConflictPolicy::Ask;
    }
    if (normalized == "skip") {
        return OutputConflictPolicy::Skip;
    }
    if (normalized == "replace") {
        return OutputConflictPolicy::Replace;
    }
    if (normalized == "unique" || normalized == "generate-unique-name") {
        return OutputConflictPolicy::GenerateUniqueName;
    }
    return std::nullopt;
}

std::string_view ToString(const ErrorCategory category) noexcept {
    switch (category) {
    case ErrorCategory::None:
        return "none";
    case ErrorCategory::Cancelled:
        return "cancelled";
    case ErrorCategory::UnsupportedFormat:
        return "unsupported_format";
    case ErrorCategory::InvalidRequest:
        return "invalid_request";
    case ErrorCategory::InvalidInput:
        return "invalid_input";
    case ErrorCategory::Io:
        return "io";
    case ErrorCategory::Codec:
        return "codec";
    case ErrorCategory::Conflict:
        return "conflict";
    case ErrorCategory::ResourceLimit:
        return "resource_limit";
    case ErrorCategory::Internal:
    default:
        return "internal";
    }
}

std::string_view ToString(const ConversionStatus status) noexcept {
    switch (status) {
    case ConversionStatus::Success:
        return "success";
    case ConversionStatus::Skipped:
        return "skipped";
    case ConversionStatus::Cancelled:
        return "cancelled";
    case ConversionStatus::Failed:
    default:
        return "failed";
    }
}

std::string_view ToString(const JobState state) noexcept {
    switch (state) {
    case JobState::Pending:
        return "pending";
    case JobState::Inspecting:
        return "inspecting";
    case JobState::Ready:
        return "ready";
    case JobState::Converting:
        return "converting";
    case JobState::Paused:
        return "paused";
    case JobState::Cancelling:
        return "cancelling";
    case JobState::Cancelled:
        return "cancelled";
    case JobState::Completed:
        return "completed";
    case JobState::CompletedWithWarnings:
        return "completed_with_warnings";
    case JobState::Failed:
    default:
        return "failed";
    }
}

} // namespace nativeshift::core
