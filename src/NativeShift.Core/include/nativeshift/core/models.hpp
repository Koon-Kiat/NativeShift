#pragma once

#include "nativeshift/core/formats.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nativeshift::core {

enum class OutputConflictPolicy { Ask, Skip, Replace, GenerateUniqueName };

enum class ErrorCategory {
    None,
    Cancelled,
    UnsupportedFormat,
    InvalidRequest,
    InvalidInput,
    Io,
    Codec,
    Conflict,
    ResourceLimit,
    Internal
};

enum class ConversionStatus { Success, Skipped, Cancelled, Failed };

enum class JobState {
    Pending,
    Inspecting,
    Ready,
    Converting,
    Paused,
    Cancelling,
    Cancelled,
    Completed,
    CompletedWithWarnings,
    Failed
};

enum class ConversionOptionKind { Boolean, Integer, Text, Choice };

enum class ImageResizeMode { Fit, Fill, Stretch };

enum class AudioCodec { Automatic, Mp3, PcmS16, Flac, Aac, Vorbis, Opus, Copy };

enum class VideoCodec { Automatic, H264, H265, Vp9, Av1, Copy };

enum class VideoAudioCodec { Automatic, Aac, Mp3, Opus, Vorbis, Copy, None };

enum class HardwareAcceleration {
    Automatic,
    PreferHardware,
    SoftwareOnly,
    Specific
};

enum class SubtitleHandling { CopyCompatible, Drop };

struct RgbaColor {
    std::uint8_t red{255};
    std::uint8_t green{255};
    std::uint8_t blue{255};
    std::uint8_t alpha{255};
};

struct ConversionOption {
    std::string key;
    std::string display_name;
    ConversionOptionKind kind{ConversionOptionKind::Text};
    bool required{false};
    std::vector<std::string> choices;
};

struct ImageOptions {
    int quality{85};
    int compression_level{6};
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    ImageResizeMode resize_mode{ImageResizeMode::Fit};
    bool prevent_enlargement{false};
    std::uint16_t rotation_degrees{0};
    bool automatic_orientation{true};
    bool lossless{false};
    bool preserve_metadata{false};
    bool preserve_color_profile{false};
    RgbaColor background;
};

struct AudioOptions {
    AudioCodec codec{AudioCodec::Automatic};
    int bitrate_kbps{192};
    bool variable_bitrate{false};
    std::optional<int> sample_rate;
    std::optional<int> channels;
    std::string sample_format;
    bool preserve_source_settings{true};
    bool preserve_metadata{false};
    std::optional<int> stream_index;
};

struct VideoOptions {
    VideoCodec video_codec{VideoCodec::Automatic};
    VideoAudioCodec audio_codec{VideoAudioCodec::Automatic};
    std::optional<int> quality;
    std::optional<int> video_bitrate_kbps;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    bool preserve_aspect_ratio{true};
    std::optional<double> frame_rate;
    int audio_bitrate_kbps{192};
    std::optional<int> audio_sample_rate;
    std::optional<int> audio_channels;
    std::string encoder_preset{"medium"};
    HardwareAcceleration hardware_acceleration{HardwareAcceleration::Automatic};
    std::string specific_encoder;
    SubtitleHandling subtitles{SubtitleHandling::CopyCompatible};
    bool preserve_metadata{false};
    bool stream_copy{false};
    std::optional<int> video_stream_index;
    std::optional<int> audio_stream_index;
};

struct ConversionRequest {
    std::uint64_t job_id{};
    std::filesystem::path input_path;
    std::filesystem::path output_path;
    FileFormat input_format{FileFormat::Unknown};
    FileFormat output_format{FileFormat::Unknown};
    OutputConflictPolicy conflict_policy{OutputConflictPolicy::Ask};
    ImageOptions image;
    AudioOptions audio;
    VideoOptions video;
};

struct ConversionProgress {
    double fraction{};
    std::string stage;
};

using ProgressCallback = std::function<void(const ConversionProgress&)>;

struct ConversionError {
    ErrorCategory category{ErrorCategory::None};
    std::string code;
    std::string message;
};

struct ValidationIssue {
    ErrorCategory category{ErrorCategory::InvalidRequest};
    std::string code;
    std::string message;
};

struct ProviderOutcome {
    bool success{false};
    bool cancelled{false};
    ErrorCategory error{ErrorCategory::None};
    std::string message;
    std::string selected_codec;
    std::string hardware_acceleration{"none"};
    std::vector<std::string> warnings;

    [[nodiscard]] static ProviderOutcome Succeeded();
    [[nodiscard]] static ProviderOutcome Failed(ErrorCategory category,
                                                std::string message);
    [[nodiscard]] static ProviderOutcome Cancelled(std::string message);
};

struct ConversionResult {
    std::uint64_t job_id{};
    ConversionStatus status{ConversionStatus::Failed};
    ErrorCategory error{ErrorCategory::Internal};
    std::string message;
    std::string provider;
    std::string selected_codec;
    std::string hardware_acceleration{"none"};
    FileFormat input_format{FileFormat::Unknown};
    FileFormat output_format{FileFormat::Unknown};
    std::filesystem::path output_path;
    std::chrono::milliseconds duration{};
    std::vector<std::string> warnings;
};

struct Job {
    std::uint64_t id{};
    JobState state{JobState::Pending};
    ConversionRequest request;
    ConversionProgress progress;
    std::optional<ConversionResult> result;
};

[[nodiscard]] std::string_view ToString(OutputConflictPolicy policy) noexcept;
[[nodiscard]] std::optional<OutputConflictPolicy>
OutputConflictPolicyFromString(std::string_view value);
[[nodiscard]] std::string_view ToString(ErrorCategory category) noexcept;
[[nodiscard]] std::string_view ToString(ConversionStatus status) noexcept;
[[nodiscard]] std::string_view ToString(JobState state) noexcept;

} // namespace nativeshift::core
