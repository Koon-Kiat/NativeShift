#include "nativeshift/core/format_detector.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <string_view>

namespace nativeshift::core {
namespace {

bool Matches(const std::span<const std::uint8_t> bytes,
             const std::size_t offset,
             const std::initializer_list<std::uint8_t> signature) {
    if (offset > bytes.size() || signature.size() > bytes.size() - offset) {
        return false;
    }
    const auto candidate = bytes.subspan(offset, signature.size());
    return std::equal(signature.begin(), signature.end(), candidate.begin());
}

bool ContainsAscii(const std::span<const std::uint8_t> bytes,
                   const std::string_view value) {
    if (value.empty() || bytes.size() < value.size()) {
        return false;
    }
    const auto first = bytes.begin();
    return std::search(first, bytes.end(), value.begin(), value.end()) !=
           bytes.end();
}

} // namespace

FileProbeResult DetectFormat(const std::filesystem::path& input_path) {
    std::ifstream stream(input_path, std::ios::binary);
    if (!stream) {
        return {FileFormat::Unknown, "The input file could not be opened."};
    }

    std::array<std::uint8_t, 4096> bytes{};
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    const auto count = static_cast<std::size_t>(stream.gcount());

    return DetectFormat(std::span<const std::uint8_t>(bytes.data(), count));
}

FileProbeResult DetectFormat(const std::span<const std::uint8_t> bytes) {
    if (Matches(bytes, 0, {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A})) {
        return {FileFormat::Png, {}};
    }
    if (Matches(bytes, 0, {0xFF, 0xD8, 0xFF})) {
        return {FileFormat::Jpeg, {}};
    }
    if (Matches(bytes, 0, {'R', 'I', 'F', 'F'}) &&
        Matches(bytes, 8, {'W', 'E', 'B', 'P'})) {
        return {FileFormat::WebP, {}};
    }
    if (Matches(bytes, 0, {'B', 'M'})) {
        return {FileFormat::Bmp, {}};
    }
    if (Matches(bytes, 0, {'I', 'I', 0x2A, 0x00}) ||
        Matches(bytes, 0, {'M', 'M', 0x00, 0x2A}) ||
        Matches(bytes, 0, {'I', 'I', 0x2B, 0x00}) ||
        Matches(bytes, 0, {'M', 'M', 0x00, 0x2B})) {
        return {FileFormat::Tiff, {}};
    }
    if (Matches(bytes, 0, {'f', 'L', 'a', 'C'})) {
        return {FileFormat::Flac, {}};
    }
    if (Matches(bytes, 0, {'O', 'g', 'g', 'S'}) &&
        ContainsAscii(bytes, "OpusHead")) {
        return {FileFormat::Opus, {}};
    }
    if (Matches(bytes, 0, {'O', 'g', 'g', 'S'})) {
        return {FileFormat::Ogg, {}};
    }
    if (Matches(bytes, 0, {'R', 'I', 'F', 'F'}) &&
        Matches(bytes, 8, {'W', 'A', 'V', 'E'})) {
        return {FileFormat::Wav, {}};
    }
    if (Matches(bytes, 0, {'R', 'I', 'F', 'F'}) &&
        Matches(bytes, 8, {'A', 'V', 'I', ' '})) {
        return {FileFormat::Avi, {}};
    }
    if (Matches(bytes, 0, {'I', 'D', '3'})) {
        return {FileFormat::Mp3, {}};
    }
    if (bytes.size() >= 2 && bytes[0] == 0xFF && (bytes[1] & 0xF6U) == 0xF0U) {
        return {FileFormat::Aac, {}};
    }
    if (bytes.size() >= 2 && bytes[0] == 0xFF && (bytes[1] & 0xE0U) == 0xE0U) {
        return {FileFormat::Mp3, {}};
    }
    if (Matches(bytes, 0, {0x1A, 0x45, 0xDF, 0xA3})) {
        return {ContainsAscii(bytes, "webm") ? FileFormat::WebM
                                             : FileFormat::Mkv,
                {}};
    }
    if (Matches(bytes, 4, {'f', 't', 'y', 'p'})) {
        if (Matches(bytes, 8, {'M', '4', 'A', ' '}) ||
            Matches(bytes, 8, {'M', '4', 'B', ' '})) {
            return {FileFormat::M4a, {}};
        }
        if (Matches(bytes, 8, {'q', 't', ' ', ' '})) {
            return {FileFormat::Mov, {}};
        }
        return {FileFormat::Mp4, {}};
    }

    return {
        FileFormat::Unknown,
        "The file signature is not recognized; the extension was not trusted."};
}

} // namespace nativeshift::core
