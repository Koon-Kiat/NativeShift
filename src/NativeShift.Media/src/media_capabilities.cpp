#include "nativeshift/media/media_capabilities.hpp"

#include "ffmpeg_support.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
}

#include <array>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace nativeshift::media {
namespace {

bool IsHardwareEncoder(const std::string_view name) {
    return name.ends_with("_nvenc") || name.ends_with("_qsv") ||
           name.ends_with("_amf") || name.ends_with("_mf");
}

MediaRuntimeCapabilities InspectCapabilities() {
    MediaRuntimeCapabilities result;
    result.ffmpeg_version = av_version_info();

    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
        const char* name = av_hwdevice_get_type_name(type);
        HardwareDeviceCapability capability;
        capability.name = name == nullptr ? "unknown" : name;
        AVBufferRef* device = nullptr;
        const int status =
            av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0);
        capability.available = status >= 0;
        if (status < 0) {
            capability.diagnostic = detail::ErrorText(status);
        }
        av_buffer_unref(&device);
        result.hardware_devices.push_back(std::move(capability));
    }

    constexpr std::array encoder_names{
        std::string_view{"h264_nvenc"},  std::string_view{"h264_qsv"},
        std::string_view{"h264_amf"},    std::string_view{"h264_mf"},
        std::string_view{"hevc_nvenc"},  std::string_view{"hevc_qsv"},
        std::string_view{"hevc_amf"},    std::string_view{"hevc_mf"},
        std::string_view{"vp9_qsv"},     std::string_view{"av1_nvenc"},
        std::string_view{"av1_qsv"},     std::string_view{"av1_amf"},
        std::string_view{"libopenh264"}, std::string_view{"libvpx-vp9"},
        std::string_view{"libaom-av1"},  std::string_view{"libmp3lame"},
        std::string_view{"aac"},         std::string_view{"flac"},
        std::string_view{"libvorbis"},   std::string_view{"libopus"},
    };
    for (const auto name : encoder_names) {
        const auto* encoder =
            avcodec_find_encoder_by_name(std::string(name).c_str());
        EncoderCapability capability;
        capability.name = name;
        capability.registered = encoder != nullptr;
        capability.hardware = IsHardwareEncoder(name);
        if (encoder != nullptr) {
            capability.codec = avcodec_get_name(encoder->id);
        }
        result.encoders.push_back(std::move(capability));
    }
    return result;
}

} // namespace

const MediaRuntimeCapabilities& DetectMediaCapabilities() {
    static std::once_flag once;
    static MediaRuntimeCapabilities capabilities;
    std::call_once(once, [] { capabilities = InspectCapabilities(); });
    return capabilities;
}

} // namespace nativeshift::media
