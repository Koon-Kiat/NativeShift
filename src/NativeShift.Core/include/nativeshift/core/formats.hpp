#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace nativeshift::core {

enum class FileFormat {
    Unknown,
    Png,
    Jpeg,
    WebP,
    Bmp,
    Tiff,
    Mp3,
    Wav,
    Flac,
    Aac,
    M4a,
    Ogg,
    Mp4,
    Mkv,
    WebM,
    Mov,
    Avi
};

enum class MediaKind { Unknown, Image, Audio, Video };

[[nodiscard]] std::string_view ToString(FileFormat format) noexcept;
[[nodiscard]] std::optional<FileFormat>
FormatFromString(std::string_view value);
[[nodiscard]] std::string_view ExtensionFor(FileFormat format) noexcept;
[[nodiscard]] MediaKind KindOf(FileFormat format) noexcept;
[[nodiscard]] bool IsImageFormat(FileFormat format) noexcept;

} // namespace nativeshift::core
