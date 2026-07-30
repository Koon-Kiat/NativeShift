#include "nativeshift/core/format_detector.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>

namespace nativeshift::core {
namespace {

bool Matches(const std::array<std::uint8_t, 16>& bytes,
             const std::size_t available, const std::size_t offset,
             const std::initializer_list<std::uint8_t> signature) {
    if (offset + signature.size() > available) {
        return false;
    }
    std::size_t index = offset;
    for (const auto expected : signature) {
        if (bytes[index] != expected) {
            return false;
        }
        ++index;
    }
    return true;
}

} // namespace

FileProbeResult DetectFormat(const std::filesystem::path& input_path) {
    std::ifstream stream(input_path, std::ios::binary);
    if (!stream) {
        return {FileFormat::Unknown, "The input file could not be opened."};
    }

    std::array<std::uint8_t, 16> bytes{};
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    const auto count = static_cast<std::size_t>(stream.gcount());

    if (Matches(bytes, count, 0,
                {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A})) {
        return {FileFormat::Png, {}};
    }
    if (Matches(bytes, count, 0, {0xFF, 0xD8, 0xFF})) {
        return {FileFormat::Jpeg, {}};
    }
    if (Matches(bytes, count, 0, {'R', 'I', 'F', 'F'}) &&
        Matches(bytes, count, 8, {'W', 'E', 'B', 'P'})) {
        return {FileFormat::WebP, {}};
    }
    if (Matches(bytes, count, 0, {'B', 'M'})) {
        return {FileFormat::Bmp, {}};
    }
    if (Matches(bytes, count, 0, {'I', 'I', 0x2A, 0x00}) ||
        Matches(bytes, count, 0, {'M', 'M', 0x00, 0x2A})) {
        return {FileFormat::Tiff, {}};
    }
    if (Matches(bytes, count, 0, {'f', 'L', 'a', 'C'})) {
        return {FileFormat::Flac, {}};
    }
    if (Matches(bytes, count, 0, {'O', 'g', 'g', 'S'})) {
        return {FileFormat::Ogg, {}};
    }
    if (Matches(bytes, count, 0, {'R', 'I', 'F', 'F'}) &&
        Matches(bytes, count, 8, {'W', 'A', 'V', 'E'})) {
        return {FileFormat::Wav, {}};
    }
    if (Matches(bytes, count, 0, {'R', 'I', 'F', 'F'}) &&
        Matches(bytes, count, 8, {'A', 'V', 'I', ' '})) {
        return {FileFormat::Avi, {}};
    }
    if (Matches(bytes, count, 0, {'I', 'D', '3'}) ||
        (count >= 2 && bytes[0] == 0xFF && (bytes[1] & 0xE0U) == 0xE0U)) {
        return {FileFormat::Mp3, {}};
    }

    return {
        FileFormat::Unknown,
        "The file signature is not recognized; the extension was not trusted."};
}

} // namespace nativeshift::core
