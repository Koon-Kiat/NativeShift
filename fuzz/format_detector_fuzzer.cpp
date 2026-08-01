#include "nativeshift/core/format_detector.hpp"
#include "nativeshift/core/formats.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      const std::size_t size) {
    const auto bytes = std::span<const std::uint8_t>(data, size);
    const auto detection = nativeshift::core::DetectFormat(bytes);

    const auto format = detection.format;
    (void)nativeshift::core::ToString(format);
    (void)nativeshift::core::ExtensionFor(format);
    (void)nativeshift::core::KindOf(format);

    const auto text =
        std::string_view(reinterpret_cast<const char*>(data), size);
    if (const auto parsed = nativeshift::core::FormatFromString(text)) {
        (void)nativeshift::core::ToString(*parsed);
        (void)nativeshift::core::ExtensionFor(*parsed);
        (void)nativeshift::core::KindOf(*parsed);
    }

    return 0;
}
