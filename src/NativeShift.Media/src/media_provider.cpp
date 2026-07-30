#include "nativeshift/media/media_provider.hpp"

#include "media_operations.hpp"

#include <array>
#include <filesystem>
#include <limits>
#include <string>

namespace nativeshift::media {
namespace {

using core::AudioCodec;
using core::FileFormat;
using core::ValidationIssue;
using core::VideoAudioCodec;
using core::VideoCodec;

ValidationIssue Invalid(std::string code, std::string message) {
    return {core::ErrorCategory::InvalidRequest, std::move(code),
            std::move(message)};
}

bool AudioCodecFits(const FileFormat output, const AudioCodec codec) {
    if (codec == AudioCodec::Automatic || codec == AudioCodec::Copy) {
        return true;
    }
    switch (output) {
    case FileFormat::Mp3:
        return codec == AudioCodec::Mp3;
    case FileFormat::Wav:
        return codec == AudioCodec::PcmS16;
    case FileFormat::Flac:
        return codec == AudioCodec::Flac;
    case FileFormat::Aac:
    case FileFormat::M4a:
        return codec == AudioCodec::Aac;
    case FileFormat::Ogg:
        return codec == AudioCodec::Vorbis;
    case FileFormat::Opus:
        return codec == AudioCodec::Opus;
    default:
        return false;
    }
}

bool VideoCodecFits(const FileFormat output, const VideoCodec codec) {
    if (codec == VideoCodec::Automatic || codec == VideoCodec::Copy) {
        return true;
    }
    if (output == FileFormat::WebM) {
        return codec == VideoCodec::Vp9 || codec == VideoCodec::Av1;
    }
    return output == FileFormat::Mp4 || output == FileFormat::Mkv;
}

bool VideoAudioCodecFits(const FileFormat output, const VideoAudioCodec codec) {
    if (codec == VideoAudioCodec::Automatic || codec == VideoAudioCodec::Copy ||
        codec == VideoAudioCodec::None) {
        return true;
    }
    if (output == FileFormat::WebM) {
        return codec == VideoAudioCodec::Opus ||
               codec == VideoAudioCodec::Vorbis;
    }
    if (output == FileFormat::Mp4) {
        return codec == VideoAudioCodec::Aac || codec == VideoAudioCodec::Mp3;
    }
    return output == FileFormat::Mkv;
}

} // namespace

std::string MediaConversionProvider::Name() const {
    return "Native FFmpeg media provider";
}

bool MediaConversionProvider::CanHandle(
    const FileFormat input, const FileFormat output) const noexcept {
    return (core::IsAudioFormat(input) && core::IsAudioFormat(output)) ||
           (core::IsVideoFormat(input) &&
            (output == FileFormat::Mp4 || output == FileFormat::Mkv ||
             output == FileFormat::WebM));
}

std::vector<ValidationIssue> MediaConversionProvider::Validate(
    const core::ConversionRequest& request) const {
    std::vector<ValidationIssue> issues;
    if (!CanHandle(request.input_format, request.output_format)) {
        issues.push_back(Invalid(
            "media.unsupported_pair",
            "The media provider does not support this format combination."));
        return issues;
    }

    if (core::IsAudioFormat(request.output_format)) {
        if (!AudioCodecFits(request.output_format, request.audio.codec)) {
            issues.push_back(Invalid(
                "audio.codec_container",
                "The selected audio codec is incompatible with the output "
                "container."));
        }
        if (request.audio.bitrate_kbps < 8 ||
            request.audio.bitrate_kbps > 1'536) {
            issues.push_back(
                Invalid("audio.bitrate", "Audio bitrate must be 8-1536 kbps."));
        }
        if (request.audio.sample_rate &&
            (*request.audio.sample_rate < 8'000 ||
             *request.audio.sample_rate > 384'000)) {
            issues.push_back(Invalid(
                "audio.sample_rate",
                "Audio sample rate must be between 8000 and 384000 Hz."));
        }
        if (request.audio.channels &&
            (*request.audio.channels < 1 || *request.audio.channels > 8)) {
            issues.push_back(
                Invalid("audio.channels",
                        "Audio channel count must be between 1 and 8."));
        }
        return issues;
    }

    if (!VideoCodecFits(request.output_format, request.video.video_codec)) {
        issues.push_back(
            Invalid("video.codec_container",
                    "The selected video codec is incompatible with the output "
                    "container."));
    }
    if (!VideoAudioCodecFits(request.output_format,
                             request.video.audio_codec)) {
        issues.push_back(
            Invalid("video.audio_codec_container",
                    "The selected audio codec is incompatible with the video "
                    "container."));
    }
    if (request.video.quality &&
        (*request.video.quality < 0 || *request.video.quality > 63)) {
        issues.push_back(Invalid("video.quality",
                                 "Video quality must be between 0 and 63."));
    }
    if (request.video.video_bitrate_kbps &&
        (*request.video.video_bitrate_kbps < 16 ||
         *request.video.video_bitrate_kbps > 1'000'000)) {
        issues.push_back(
            Invalid("video.bitrate", "Video bitrate must be 16-1000000 kbps."));
    }
    if ((request.video.width &&
         (*request.video.width == 0 || *request.video.width > 16'384)) ||
        (request.video.height &&
         (*request.video.height == 0 || *request.video.height > 16'384))) {
        issues.push_back(
            Invalid("video.dimensions",
                    "Video dimensions must be between 1 and 16384 pixels."));
    }
    if (request.video.frame_rate && (*request.video.frame_rate <= 0.0 ||
                                     *request.video.frame_rate > 240.0)) {
        issues.push_back(
            Invalid("video.frame_rate",
                    "Video frame rate must be above 0 and at most 240."));
    }
    if (request.video.hardware_acceleration ==
            core::HardwareAcceleration::Specific &&
        request.video.specific_encoder.empty()) {
        issues.push_back(Invalid(
            "video.specific_encoder",
            "A specific encoder name is required for specific hardware mode."));
    }
    if (request.video.specific_encoder.size() > 128 ||
        request.video.specific_encoder.find_first_of("\r\n\t") !=
            std::string::npos) {
        issues.push_back(
            Invalid("video.encoder_name",
                    "The specific encoder name contains invalid characters."));
    }
    return issues;
}

std::uintmax_t MediaConversionProvider::EstimateOutput(
    const core::ConversionRequest& request) const {
    std::error_code error;
    const auto input_size =
        std::filesystem::file_size(request.input_path, error);
    if (error) {
        return 512ULL * 1024ULL * 1024ULL;
    }
    const auto maximum = std::numeric_limits<std::uintmax_t>::max();
    if (input_size > maximum / 2U) {
        return maximum;
    }
    return input_size + input_size / 2U + 16ULL * 1024ULL * 1024ULL;
}

core::ProviderOutcome
MediaConversionProvider::Convert(const core::ConversionRequest& request,
                                 const core::ProgressCallback& progress,
                                 const std::stop_token cancellation) {
    if (core::IsAudioFormat(request.output_format)) {
        return TranscodeAudio(request, progress, cancellation);
    }
    return TranscodeVideo(request, progress, cancellation);
}

std::vector<core::FormatPair>
MediaConversionProvider::GetSupportedFormats() const {
    constexpr std::array audio_inputs{
        FileFormat::Mp3, FileFormat::Wav, FileFormat::Flac, FileFormat::Aac,
        FileFormat::M4a, FileFormat::Ogg, FileFormat::Opus,
    };
    constexpr std::array audio_outputs{
        FileFormat::Mp3, FileFormat::Wav, FileFormat::Flac, FileFormat::Aac,
        FileFormat::M4a, FileFormat::Ogg, FileFormat::Opus,
    };
    constexpr std::array video_inputs{
        FileFormat::Mp4, FileFormat::Mkv,  FileFormat::Mov,
        FileFormat::Avi, FileFormat::WebM,
    };
    constexpr std::array video_outputs{
        FileFormat::Mp4,
        FileFormat::Mkv,
        FileFormat::WebM,
    };

    std::vector<core::FormatPair> formats;
    formats.reserve(audio_inputs.size() * audio_outputs.size() +
                    video_inputs.size() * video_outputs.size());
    for (const auto input : audio_inputs) {
        for (const auto output : audio_outputs) {
            formats.push_back({input, output});
        }
    }
    for (const auto input : video_inputs) {
        for (const auto output : video_outputs) {
            formats.push_back({input, output});
        }
    }
    return formats;
}

std::vector<std::string> MediaConversionProvider::GetAvailableOptions() const {
    return {
        "audio-codec",      "audio-bitrate", "audio-vbr",   "sample-rate",
        "channels",         "metadata",      "video-codec", "video-quality",
        "video-bitrate",    "resolution",    "frame-rate",  "encoder-preset",
        "hardware-encoder", "stream-copy",   "subtitles",   "stream-selection",
    };
}

} // namespace nativeshift::media
