#pragma once

#include <string>
#include <vector>

namespace nativeshift::media {

struct HardwareDeviceCapability {
    std::string name;
    bool available{false};
    std::string diagnostic;
};

struct EncoderCapability {
    std::string name;
    std::string codec;
    bool registered{false};
    bool hardware{false};
};

struct MediaRuntimeCapabilities {
    std::string ffmpeg_version;
    std::vector<HardwareDeviceCapability> hardware_devices;
    std::vector<EncoderCapability> encoders;
};

[[nodiscard]] const MediaRuntimeCapabilities& DetectMediaCapabilities();

} // namespace nativeshift::media
