#include "nativeshift/core/format_detector.hpp"
#include "nativeshift/core/job_queue.hpp"
#include "nativeshift/core/logger.hpp"
#include "nativeshift/core/output_paths.hpp"
#include "nativeshift/core/presets.hpp"
#include "nativeshift/image/image_provider.hpp"
#include "nativeshift/media/media_capabilities.hpp"
#include "nativeshift/media/media_provider.hpp"
#include "nativeshift/platform/windows_platform_services.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using nativeshift::core::ConversionRequest;
using nativeshift::core::ConversionResult;
using nativeshift::core::ConversionStatus;
using nativeshift::core::FileFormat;
using nativeshift::core::OutputConflictPolicy;

std::atomic_bool interrupted{false};

void HandleInterrupt(int) {
    interrupted.store(true, std::memory_order_relaxed);
}

struct CliOptions {
    std::filesystem::path input;
    std::optional<std::filesystem::path> output;
    FileFormat output_format{FileFormat::Unknown};
    OutputConflictPolicy conflict_policy{
        OutputConflictPolicy::GenerateUniqueName};
    nativeshift::core::ImageOptions image;
    nativeshift::core::AudioOptions audio;
    nativeshift::core::VideoOptions video;
    std::size_t jobs{nativeshift::core::JobQueue::SafeDefaultConcurrency()};
    bool recursive{false};
    bool json{false};
    bool help{false};
    bool version{false};
    bool list_formats{false};
    bool list_codecs{false};
    bool list_hardware{false};
    bool list_presets{false};
    bool capabilities{false};
    bool verbose{false};
    bool quiet{false};
};

struct ParseResult {
    std::optional<CliOptions> options;
    std::string error;
};

std::string NarrowAscii(const std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t ch : value) {
        if (ch > 0x7F) {
            return {};
        }
        result.push_back(static_cast<char>(ch));
    }
    return result;
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::optional<std::uint32_t> ParseDimension(const std::wstring_view value) {
    try {
        std::size_t consumed = 0;
        const auto number = std::stoull(std::wstring(value), &consumed);
        if (consumed != value.size() ||
            number > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(number);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<int> ParseInteger(const std::wstring_view value) {
    try {
        std::size_t consumed = 0;
        const int number = std::stoi(std::wstring(value), &consumed);
        if (consumed != value.size()) {
            return std::nullopt;
        }
        return number;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<int> ParseBitrate(std::wstring_view value) {
    if (!value.empty() && (value.back() == L'k' || value.back() == L'K')) {
        value.remove_suffix(1);
    }
    return ParseInteger(value);
}

std::optional<double> ParseDouble(const std::wstring_view value) {
    try {
        std::size_t consumed = 0;
        const double number = std::stod(std::wstring(value), &consumed);
        if (consumed != value.size() || !std::isfinite(number)) {
            return std::nullopt;
        }
        return number;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<nativeshift::core::AudioCodec>
ParseAudioCodec(const std::string_view value) {
    using nativeshift::core::AudioCodec;
    if (value == "auto") {
        return AudioCodec::Automatic;
    }
    if (value == "mp3") {
        return AudioCodec::Mp3;
    }
    if (value == "pcm" || value == "pcm_s16le") {
        return AudioCodec::PcmS16;
    }
    if (value == "flac") {
        return AudioCodec::Flac;
    }
    if (value == "aac") {
        return AudioCodec::Aac;
    }
    if (value == "vorbis") {
        return AudioCodec::Vorbis;
    }
    if (value == "opus") {
        return AudioCodec::Opus;
    }
    if (value == "copy") {
        return AudioCodec::Copy;
    }
    return std::nullopt;
}

std::optional<nativeshift::core::VideoCodec>
ParseVideoCodec(const std::string_view value) {
    using nativeshift::core::VideoCodec;
    if (value == "auto") {
        return VideoCodec::Automatic;
    }
    if (value == "h264") {
        return VideoCodec::H264;
    }
    if (value == "h265" || value == "hevc") {
        return VideoCodec::H265;
    }
    if (value == "vp9") {
        return VideoCodec::Vp9;
    }
    if (value == "av1") {
        return VideoCodec::Av1;
    }
    if (value == "copy") {
        return VideoCodec::Copy;
    }
    return std::nullopt;
}

std::optional<nativeshift::core::VideoAudioCodec>
ParseVideoAudioCodec(const std::string_view value) {
    using nativeshift::core::VideoAudioCodec;
    if (value == "auto") {
        return VideoAudioCodec::Automatic;
    }
    if (value == "aac") {
        return VideoAudioCodec::Aac;
    }
    if (value == "mp3") {
        return VideoAudioCodec::Mp3;
    }
    if (value == "opus") {
        return VideoAudioCodec::Opus;
    }
    if (value == "vorbis") {
        return VideoAudioCodec::Vorbis;
    }
    if (value == "copy") {
        return VideoAudioCodec::Copy;
    }
    if (value == "none") {
        return VideoAudioCodec::None;
    }
    return std::nullopt;
}

std::optional<nativeshift::core::RgbaColor>
ParseColor(std::wstring_view value) {
    if (!value.empty() && value.front() == L'#') {
        value.remove_prefix(1);
    }
    if (value.size() != 6) {
        return std::nullopt;
    }
    try {
        std::size_t consumed = 0;
        const auto color = std::stoul(std::wstring(value), &consumed, 16);
        if (consumed != value.size()) {
            return std::nullopt;
        }
        return nativeshift::core::RgbaColor{
            static_cast<std::uint8_t>((color >> 16U) & 0xFFU),
            static_cast<std::uint8_t>((color >> 8U) & 0xFFU),
            static_cast<std::uint8_t>(color & 0xFFU),
            255,
        };
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool ApplyPreset(CliOptions& options, const std::string_view name,
                 std::string& error) {
    auto presets = nativeshift::core::PresetStore::BuiltIns();
    const auto custom = nativeshift::core::PresetStore().LoadCustom();
    presets.insert(presets.end(), custom.presets.begin(), custom.presets.end());
    const auto found =
        std::ranges::find_if(presets, [name](const auto& preset) {
            return preset.id == name || preset.name == name;
        });
    if (found == presets.end()) {
        error = "The requested conversion preset was not found.";
        return false;
    }
    options.output_format = found->output_format;
    options.image = found->image;
    options.audio = found->audio;
    options.video = found->video;
    return true;
}

ParseResult ParseArguments(const int argc, wchar_t* argv[]) {
    CliOptions options;
    if (argc <= 1) {
        return {std::nullopt, "An input file or folder is required."};
    }

    for (int index = 1; index < argc; ++index) {
        if (std::wstring_view(argv[index]) != L"--preset") {
            continue;
        }
        if (index + 1 >= argc) {
            return {std::nullopt, "--preset requires a preset ID or name."};
        }
        std::string error;
        if (!ApplyPreset(options, NarrowAscii(argv[index + 1]), error)) {
            return {std::nullopt, std::move(error)};
        }
        ++index;
    }

    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        const auto require_value =
            [&](const wchar_t* name) -> std::optional<std::wstring_view> {
            if (index + 1 >= argc) {
                return std::nullopt;
            }
            ++index;
            (void)name;
            return std::wstring_view(argv[index]);
        };

        if (argument == L"--help" || argument == L"-h") {
            options.help = true;
        } else if (argument == L"--version") {
            options.version = true;
        } else if (argument == L"--list-formats") {
            options.list_formats = true;
        } else if (argument == L"--list-codecs") {
            options.list_codecs = true;
        } else if (argument == L"--list-hardware") {
            options.list_hardware = true;
        } else if (argument == L"--list-presets") {
            options.list_presets = true;
        } else if (argument == L"--capabilities") {
            options.capabilities = true;
        } else if (argument == L"--to") {
            const auto value = require_value(L"--to");
            if (!value) {
                return {std::nullopt, "--to requires a format."};
            }
            const auto format =
                nativeshift::core::FormatFromString(NarrowAscii(*value));
            if (!format || *format == FileFormat::Mov ||
                *format == FileFormat::Avi) {
                return {std::nullopt, "--to is not a supported output format."};
            }
            options.output_format = *format;
        } else if (argument == L"--output" || argument == L"-o") {
            const auto value = require_value(L"--output");
            if (!value) {
                return {std::nullopt, "--output requires a path."};
            }
            options.output = std::filesystem::path(*value);
        } else if (argument == L"--quality") {
            const auto value = require_value(L"--quality");
            const auto number = value ? ParseInteger(*value) : std::nullopt;
            if (!number) {
                return {std::nullopt, "--quality requires an integer."};
            }
            options.image.quality = *number;
        } else if (argument == L"--width") {
            const auto value = require_value(L"--width");
            const auto number = value ? ParseDimension(*value) : std::nullopt;
            if (!number) {
                return {std::nullopt, "--width requires a positive integer."};
            }
            options.image.width = *number;
            options.video.width = *number;
        } else if (argument == L"--height") {
            const auto value = require_value(L"--height");
            const auto number = value ? ParseDimension(*value) : std::nullopt;
            if (!number) {
                return {std::nullopt, "--height requires a positive integer."};
            }
            options.image.height = *number;
            options.video.height = *number;
        } else if (argument == L"--lossless") {
            options.image.lossless = true;
        } else if (argument == L"--fit") {
            options.image.resize_mode = nativeshift::core::ImageResizeMode::Fit;
        } else if (argument == L"--fill") {
            options.image.resize_mode =
                nativeshift::core::ImageResizeMode::Fill;
        } else if (argument == L"--stretch") {
            options.image.resize_mode =
                nativeshift::core::ImageResizeMode::Stretch;
        } else if (argument == L"--prevent-enlargement") {
            options.image.prevent_enlargement = true;
        } else if (argument == L"--rotate") {
            const auto value = require_value(L"--rotate");
            const auto number = value ? ParseInteger(*value) : std::nullopt;
            if (!number || (*number != 0 && *number != 90 && *number != 180 &&
                            *number != 270)) {
                return {std::nullopt, "--rotate must be 0, 90, 180, or 270."};
            }
            options.image.rotation_degrees =
                static_cast<std::uint16_t>(*number);
        } else if (argument == L"--no-auto-orient") {
            options.image.automatic_orientation = false;
        } else if (argument == L"--preserve-metadata") {
            options.image.preserve_metadata = true;
            options.audio.preserve_metadata = true;
            options.video.preserve_metadata = true;
        } else if (argument == L"--remove-metadata") {
            options.image.preserve_metadata = false;
            options.audio.preserve_metadata = false;
            options.video.preserve_metadata = false;
        } else if (argument == L"--preserve-color-profile") {
            options.image.preserve_color_profile = true;
        } else if (argument == L"--background") {
            const auto value = require_value(L"--background");
            const auto color = value ? ParseColor(*value) : std::nullopt;
            if (!color) {
                return {std::nullopt,
                        "--background requires an RRGGBB hexadecimal colour."};
            }
            options.image.background = *color;
        } else if (argument == L"--compression-level") {
            const auto value = require_value(L"--compression-level");
            const auto number = value ? ParseInteger(*value) : std::nullopt;
            if (!number || *number < 0 || *number > 9) {
                return {std::nullopt,
                        "--compression-level must be between 0 and 9."};
            }
            options.image.compression_level = *number;
        } else if (argument == L"--audio-codec") {
            const auto value = require_value(L"--audio-codec");
            const auto codec =
                value ? ParseAudioCodec(NarrowAscii(*value)) : std::nullopt;
            if (!codec) {
                return {std::nullopt,
                        "--audio-codec must be auto, mp3, pcm_s16le, flac, "
                        "aac, vorbis, opus, or copy."};
            }
            options.audio.codec = *codec;
        } else if (argument == L"--audio-bitrate") {
            const auto value = require_value(L"--audio-bitrate");
            const auto number = value ? ParseBitrate(*value) : std::nullopt;
            if (!number) {
                return {std::nullopt,
                        "--audio-bitrate requires an integer in kbps."};
            }
            options.audio.bitrate_kbps = *number;
            options.video.audio_bitrate_kbps = *number;
        } else if (argument == L"--vbr") {
            options.audio.variable_bitrate = true;
        } else if (argument == L"--sample-rate") {
            const auto value = require_value(L"--sample-rate");
            const auto number = value ? ParseInteger(*value) : std::nullopt;
            if (!number) {
                return {std::nullopt,
                        "--sample-rate requires an integer in Hz."};
            }
            options.audio.sample_rate = *number;
            options.video.audio_sample_rate = *number;
        } else if (argument == L"--channels") {
            const auto value = require_value(L"--channels");
            const auto number = value ? ParseInteger(*value) : std::nullopt;
            if (!number) {
                return {std::nullopt, "--channels requires an integer."};
            }
            options.audio.channels = *number;
            options.video.audio_channels = *number;
        } else if (argument == L"--sample-format") {
            const auto value = require_value(L"--sample-format");
            if (!value || NarrowAscii(*value).empty()) {
                return {std::nullopt, "--sample-format requires a value."};
            }
            options.audio.sample_format = NarrowAscii(*value);
        } else if (argument == L"--video-codec") {
            const auto value = require_value(L"--video-codec");
            const auto codec =
                value ? ParseVideoCodec(NarrowAscii(*value)) : std::nullopt;
            if (!codec) {
                return {std::nullopt,
                        "--video-codec must be auto, h264, h265, vp9, av1, or "
                        "copy."};
            }
            options.video.video_codec = *codec;
        } else if (argument == L"--video-audio-codec") {
            const auto value = require_value(L"--video-audio-codec");
            const auto codec = value ? ParseVideoAudioCodec(NarrowAscii(*value))
                                     : std::nullopt;
            if (!codec) {
                return {std::nullopt,
                        "--video-audio-codec must be auto, aac, mp3, opus, "
                        "vorbis, copy, or none."};
            }
            options.video.audio_codec = *codec;
        } else if (argument == L"--video-bitrate") {
            const auto value = require_value(L"--video-bitrate");
            const auto number = value ? ParseBitrate(*value) : std::nullopt;
            if (!number) {
                return {std::nullopt,
                        "--video-bitrate requires an integer in kbps."};
            }
            options.video.video_bitrate_kbps = *number;
        } else if (argument == L"--video-quality") {
            const auto value = require_value(L"--video-quality");
            const auto number = value ? ParseInteger(*value) : std::nullopt;
            if (!number) {
                return {std::nullopt, "--video-quality requires an integer."};
            }
            options.video.quality = *number;
        } else if (argument == L"--frame-rate") {
            const auto value = require_value(L"--frame-rate");
            const auto number = value ? ParseDouble(*value) : std::nullopt;
            if (!number) {
                return {std::nullopt, "--frame-rate requires a number."};
            }
            options.video.frame_rate = *number;
        } else if (argument == L"--encoder-preset") {
            const auto value = require_value(L"--preset");
            if (!value || NarrowAscii(*value).empty()) {
                return {std::nullopt, "--encoder-preset requires a value."};
            }
            options.video.encoder_preset = NarrowAscii(*value);
        } else if (argument == L"--preset") {
            const auto value = require_value(L"--preset");
            if (!value) {
                return {std::nullopt, "--preset requires a preset ID or name."};
            }
        } else if (argument == L"--hardware") {
            const auto value = require_value(L"--hardware");
            const auto mode = value ? NarrowAscii(*value) : std::string{};
            if (mode == "auto" || mode == "automatic") {
                options.video.hardware_acceleration =
                    nativeshift::core::HardwareAcceleration::Automatic;
            } else if (mode == "prefer") {
                options.video.hardware_acceleration =
                    nativeshift::core::HardwareAcceleration::PreferHardware;
            } else if (mode == "software") {
                options.video.hardware_acceleration =
                    nativeshift::core::HardwareAcceleration::SoftwareOnly;
            } else {
                return {std::nullopt,
                        "--hardware must be automatic, prefer, or software."};
            }
        } else if (argument == L"--encoder") {
            const auto value = require_value(L"--encoder");
            const auto encoder = value ? NarrowAscii(*value) : std::string{};
            if (encoder.empty()) {
                return {std::nullopt, "--encoder requires an FFmpeg encoder."};
            }
            options.video.hardware_acceleration =
                nativeshift::core::HardwareAcceleration::Specific;
            options.video.specific_encoder = encoder;
        } else if (argument == L"--stream-copy") {
            options.video.stream_copy = true;
            options.video.video_codec = nativeshift::core::VideoCodec::Copy;
        } else if (argument == L"--drop-subtitles") {
            options.video.subtitles = nativeshift::core::SubtitleHandling::Drop;
        } else if (argument == L"--recursive") {
            options.recursive = true;
        } else if (argument == L"--json") {
            options.json = true;
        } else if (argument == L"--verbose") {
            options.verbose = true;
        } else if (argument == L"--quiet") {
            options.quiet = true;
        } else if (argument == L"--conflict") {
            const auto value = require_value(L"--conflict");
            if (!value) {
                return {std::nullopt, "--conflict requires a policy."};
            }
            const auto policy =
                nativeshift::core::OutputConflictPolicyFromString(
                    NarrowAscii(*value));
            if (!policy) {
                return {std::nullopt,
                        "--conflict must be ask, skip, replace, or unique."};
            }
            options.conflict_policy = *policy;
        } else if (argument == L"--jobs") {
            const auto value = require_value(L"--jobs");
            const auto number = value ? ParseInteger(*value) : std::nullopt;
            if (!number || *number < 1 || *number > 32) {
                return {std::nullopt, "--jobs must be between 1 and 32."};
            }
            options.jobs = static_cast<std::size_t>(*number);
        } else if (argument.starts_with(L"-")) {
            return {std::nullopt, "Unknown option: " + NarrowAscii(argument)};
        } else if (options.input.empty()) {
            options.input = std::filesystem::path(argument);
        } else {
            return {std::nullopt, "Only one input file or folder is accepted."};
        }
    }

    const bool informational = options.help || options.version ||
                               options.list_formats || options.list_codecs ||
                               options.list_hardware || options.list_presets ||
                               options.capabilities;
    if (options.verbose && options.quiet) {
        return {std::nullopt, "--verbose and --quiet cannot be used together."};
    }
    if (!informational && options.input.empty()) {
        return {std::nullopt, "An input file or folder is required."};
    }
    if (!informational && options.output_format == FileFormat::Unknown) {
        return {std::nullopt, "--to is required."};
    }
    return {std::move(options), {}};
}

void PrintUsage() {
    std::cout
        << "NativeShift CLI " << NATIVESHIFT_VERSION << "\n\n"
        << "Usage:\n"
        << "  nativeshift-cli <input> --to "
           "<format> [options]\n"
        << "  nativeshift-cli --list-formats [--json]\n\n"
        << "Options:\n"
        << "  -o, --output <path>       Output file or folder\n"
        << "      --quality <1-100>     JPEG/WebP quality (default 85)\n"
        << "      --lossless            Use lossless WebP encoding\n"
        << "      --width <pixels>      Optional output width\n"
        << "      --height <pixels>     Optional output height\n"
        << "      --fit|--fill|--stretch Resize mode (default fit)\n"
        << "      --prevent-enlargement Do not upscale the image\n"
        << "      --rotate <degrees>     0, 90, 180, or 270\n"
        << "      --no-auto-orient      Ignore EXIF orientation\n"
        << "      --preserve-metadata   Request metadata preservation\n"
        << "      --remove-metadata     Remove metadata (default)\n"
        << "      --preserve-color-profile Request profile preservation\n"
        << "      --background <RRGGBB> Alpha background (default white)\n"
        << "      --compression-level <0-9> PNG/TIFF compression\n"
        << "      --audio-codec <codec> Audio codec or copy\n"
        << "      --audio-bitrate <kbps> Audio bitrate\n"
        << "      --vbr                 Prefer variable-bitrate audio\n"
        << "      --sample-rate <Hz>    Output audio sample rate\n"
        << "      --channels <count>    Output audio channel count\n"
        << "      --sample-format <fmt> FFmpeg audio sample format\n"
        << "      --video-codec <codec> auto|h264|h265|vp9|av1|copy\n"
        << "      --video-audio-codec <codec> Output video audio codec\n"
        << "      --video-bitrate <kbps> Target video bitrate\n"
        << "      --video-quality <0-63> Quality-based video encoding\n"
        << "      --frame-rate <fps>    Output frame rate\n"
        << "      --preset <id>         Apply a conversion preset\n"
        << "      --encoder-preset <name> FFmpeg speed/quality preset\n"
        << "      --hardware <mode>     automatic|prefer|software\n"
        << "      --encoder <name>      Require a specific FFmpeg encoder\n"
        << "      --stream-copy         Remux compatible streams\n"
        << "      --drop-subtitles      Do not copy subtitle streams\n"
        << "      --conflict <policy>   ask|skip|replace|unique\n"
        << "      --jobs <1-32>         Maximum concurrent jobs\n"
        << "      --recursive           Recurse into an input folder\n"
        << "      --json                Emit machine-readable results\n"
        << "      --verbose             Show conversion progress and debug "
           "logs\n"
        << "      --quiet               Suppress successful result messages\n"
        << "      --list-formats        List supported conversion pairs\n"
        << "      --list-codecs         List detected FFmpeg encoders\n"
        << "      --list-hardware       List detected hardware devices\n"
        << "      --list-presets        List built-in and custom presets\n"
        << "      --capabilities        Inspect codecs and hardware devices\n"
        << "      --version             Show application version\n"
        << "  -h, --help                Show this help\n";
}

void PrintCapabilities(const bool json, const bool include_encoders = true,
                       const bool include_hardware = true) {
    const auto& capabilities = nativeshift::media::DetectMediaCapabilities();
    if (json) {
        nlohmann::json output{
            {"schema_version", 1},
            {"application", "NativeShift"},
            {"version", NATIVESHIFT_VERSION},
            {"ffmpeg_version", capabilities.ffmpeg_version},
            {"hardware_devices", nlohmann::json::array()},
            {"encoders", nlohmann::json::array()},
        };
        if (include_hardware) {
            for (const auto& device : capabilities.hardware_devices) {
                output["hardware_devices"].push_back(
                    {{"name", device.name},
                     {"available", device.available},
                     {"diagnostic", device.diagnostic}});
            }
        }
        if (include_encoders) {
            for (const auto& encoder : capabilities.encoders) {
                output["encoders"].push_back(
                    {{"name", encoder.name},
                     {"codec", encoder.codec},
                     {"registered", encoder.registered},
                     {"hardware", encoder.hardware}});
            }
        }
        std::cout << output.dump() << '\n';
        return;
    }
    std::cout << "FFmpeg " << capabilities.ffmpeg_version << '\n';
    if (include_hardware) {
        for (const auto& device : capabilities.hardware_devices) {
            std::cout << "Hardware device " << device.name << ": "
                      << (device.available ? "available" : "unavailable")
                      << '\n';
        }
    }
    if (include_encoders) {
        for (const auto& encoder : capabilities.encoders) {
            if (encoder.registered) {
                std::cout << "Encoder " << encoder.name << " (" << encoder.codec
                          << ")\n";
            }
        }
    }
}

void PrintPresets(const bool json) {
    auto presets = nativeshift::core::PresetStore::BuiltIns();
    const auto custom = nativeshift::core::PresetStore().LoadCustom();
    presets.insert(presets.end(), custom.presets.begin(), custom.presets.end());
    if (json) {
        nlohmann::json output{
            {"schema_version", 1},
            {"application", "NativeShift"},
            {"version", NATIVESHIFT_VERSION},
            {"presets", nlohmann::json::array()},
        };
        for (const auto& preset : presets) {
            output["presets"].push_back(
                {{"id", preset.id},
                 {"name", preset.name},
                 {"media_kind", static_cast<int>(preset.media_kind)},
                 {"output_format",
                  nativeshift::core::ToString(preset.output_format)},
                 {"built_in", preset.built_in}});
        }
        std::cout << output.dump() << '\n';
        return;
    }
    for (const auto& preset : presets) {
        std::cout << preset.id << " - " << preset.name << " -> "
                  << nativeshift::core::ToString(preset.output_format)
                  << (preset.built_in ? " (built-in)" : " (custom)") << '\n';
    }
    if (!custom.warning.empty()) {
        std::cerr << "Warning: " << custom.warning << '\n';
    }
}

void PrintFormats(const bool json) {
    constexpr std::array image_formats{
        std::string_view{"png"},  std::string_view{"jpeg"},
        std::string_view{"webp"}, std::string_view{"bmp"},
        std::string_view{"tiff"},
    };
    constexpr std::array audio_formats{
        std::string_view{"mp3"},  std::string_view{"wav"},
        std::string_view{"flac"}, std::string_view{"aac"},
        std::string_view{"m4a"},  std::string_view{"ogg"},
        std::string_view{"opus"},
    };
    constexpr std::array video_inputs{
        std::string_view{"mp4"},  std::string_view{"mkv"},
        std::string_view{"mov"},  std::string_view{"avi"},
        std::string_view{"webm"},
    };
    constexpr std::array video_outputs{
        std::string_view{"mp4"},
        std::string_view{"mkv"},
        std::string_view{"webm"},
    };
    if (json) {
        nlohmann::json output{
            {"schema_version", 1},
            {"application", "NativeShift"},
            {"version", NATIVESHIFT_VERSION},
            {"input_formats",
             {"png", "jpeg", "webp", "bmp", "tiff", "mp3", "wav", "flac", "aac",
              "m4a", "ogg", "opus", "mp4", "mkv", "mov", "avi", "webm"}},
            {"output_formats",
             {"png", "jpeg", "webp", "bmp", "tiff", "mp3", "wav", "flac", "aac",
              "m4a", "ogg", "opus", "mp4", "mkv", "webm"}},
            {"image_input_formats", image_formats},
            {"image_output_formats", image_formats},
            {"audio_input_formats", audio_formats},
            {"audio_output_formats", audio_formats},
            {"video_input_formats", video_inputs},
            {"video_output_formats", video_outputs},
        };
        std::cout << output.dump() << '\n';
        return;
    }
    std::cout << "Image input/output: png, jpeg, webp, bmp, tiff\n"
              << "Audio input/output: mp3, wav, flac, aac, m4a, ogg, opus\n"
              << "Video input: mp4, mkv, mov, avi, webm\n"
              << "Video output: mp4, mkv, webm\n";
}

std::vector<std::filesystem::path> CollectInputs(const CliOptions& options,
                                                 std::string& error) {
    std::error_code filesystem_error;
    if (!std::filesystem::exists(options.input, filesystem_error) ||
        filesystem_error) {
        error = "The input path does not exist or is inaccessible.";
        return {};
    }
    if (std::filesystem::is_regular_file(options.input, filesystem_error)) {
        return {options.input};
    }
    if (!std::filesystem::is_directory(options.input, filesystem_error)) {
        error = "The input path is not a regular file or directory.";
        return {};
    }

    std::vector<std::filesystem::path> inputs;
    const auto inspect = [&inputs](
                             const std::filesystem::directory_entry& entry) {
        std::error_code entry_error;
        if (!entry.is_regular_file(entry_error) || entry_error) {
            return;
        }
        const auto detection = nativeshift::core::DetectFormat(entry.path());
        if (detection.format != FileFormat::Unknown) {
            inputs.push_back(entry.path());
        }
    };

    if (options.recursive) {
        const auto output_root =
            options.output.value_or(options.input / "converted");
        std::filesystem::recursive_directory_iterator iterator(
            options.input,
            std::filesystem::directory_options::skip_permission_denied,
            filesystem_error);
        const std::filesystem::recursive_directory_iterator end;
        while (!filesystem_error && iterator != end) {
            std::error_code comparison_error;
            if (iterator->is_directory(comparison_error) && !comparison_error &&
                std::filesystem::exists(output_root, comparison_error) &&
                !comparison_error &&
                std::filesystem::equivalent(iterator->path(), output_root,
                                            comparison_error) &&
                !comparison_error) {
                iterator.disable_recursion_pending();
                iterator.increment(filesystem_error);
                continue;
            }
            inspect(*iterator);
            iterator.increment(filesystem_error);
        }
    } else {
        std::filesystem::directory_iterator iterator(
            options.input,
            std::filesystem::directory_options::skip_permission_denied,
            filesystem_error);
        const std::filesystem::directory_iterator end;
        while (!filesystem_error && iterator != end) {
            inspect(*iterator);
            iterator.increment(filesystem_error);
        }
    }
    if (filesystem_error) {
        error = "The input folder could not be completely enumerated.";
        return {};
    }
    if (inputs.empty()) {
        error = "No supported media files were found.";
    }
    return inputs;
}

std::optional<std::filesystem::path>
RelativeInputParent(const std::filesystem::path& input,
                    const std::filesystem::path& root) {
    std::error_code error;
    const auto relative =
        std::filesystem::relative(input.parent_path(), root, error);
    if (error || relative.empty() || relative == ".") {
        return std::filesystem::path{};
    }
    for (const auto& part : relative) {
        if (part == "..") {
            return std::nullopt;
        }
    }
    return relative;
}

std::optional<std::vector<ConversionRequest>>
BuildRequests(const CliOptions& options,
              const std::vector<std::filesystem::path>& inputs,
              std::string& error) {
    std::error_code filesystem_error;
    const bool batch =
        std::filesystem::is_directory(options.input, filesystem_error);
    std::filesystem::path output_root;
    if (batch) {
        output_root = options.output.value_or(options.input / "converted");
        if (std::filesystem::exists(output_root, filesystem_error) &&
            !std::filesystem::is_directory(output_root, filesystem_error)) {
            error = "Batch output must be a directory.";
            return std::nullopt;
        }
        std::filesystem::create_directories(output_root, filesystem_error);
        if (filesystem_error) {
            error = "The batch output directory could not be created.";
            return std::nullopt;
        }
    }

    std::vector<ConversionRequest> requests;
    requests.reserve(inputs.size());
    for (const auto& input : inputs) {
        std::filesystem::path output;
        if (batch) {
            auto directory = output_root;
            if (options.recursive) {
                const auto relative = RelativeInputParent(input, options.input);
                if (!relative) {
                    error =
                        "An input path escaped the selected folder boundary.";
                    return std::nullopt;
                }
                directory /= *relative;
                std::filesystem::create_directories(directory,
                                                    filesystem_error);
                if (filesystem_error) {
                    error = "An output subdirectory could not be created.";
                    return std::nullopt;
                }
            }
            output = nativeshift::core::BuildOutputPath(input, directory,
                                                        options.output_format);
        } else if (!options.output) {
            output = nativeshift::core::BuildOutputPath(
                input, input.parent_path(), options.output_format);
        } else if (std::filesystem::is_directory(*options.output,
                                                 filesystem_error)) {
            output = nativeshift::core::BuildOutputPath(input, *options.output,
                                                        options.output_format);
        } else {
            output = *options.output;
        }

        ConversionRequest request;
        request.input_path = input;
        request.output_path = std::move(output);
        request.output_format = options.output_format;
        request.conflict_policy = options.conflict_policy;
        request.image = options.image;
        request.audio = options.audio;
        request.video = options.video;
        requests.push_back(std::move(request));
    }
    return requests;
}

nlohmann::json ResultJson(const ConversionResult& result) {
    nlohmann::json json{
        {"job_id", result.job_id},
        {"status", nativeshift::core::ToString(result.status)},
        {"error_category", nativeshift::core::ToString(result.error)},
        {"message", result.message},
        {"provider", result.provider},
        {"selected_codec", result.selected_codec},
        {"hardware_acceleration", result.hardware_acceleration},
        {"input_format", nativeshift::core::ToString(result.input_format)},
        {"output_format", nativeshift::core::ToString(result.output_format)},
        {"duration_ms", result.duration.count()},
        {"warnings", result.warnings},
    };
    if (!result.output_path.empty()) {
        json["output_path"] = PathToUtf8(result.output_path);
    }
    return json;
}

int ExitCodeFor(const std::vector<ConversionResult>& results) {
    bool failed = false;
    bool cancelled = false;
    bool invalid = false;
    for (const auto& result : results) {
        failed |= result.status == ConversionStatus::Failed;
        cancelled |= result.status == ConversionStatus::Cancelled;
        invalid |=
            result.error == nativeshift::core::ErrorCategory::InvalidInput ||
            result.error == nativeshift::core::ErrorCategory::InvalidRequest ||
            result.error == nativeshift::core::ErrorCategory::UnsupportedFormat;
    }
    if (cancelled) {
        return 130;
    }
    if (invalid) {
        return 3;
    }
    if (failed) {
        return 4;
    }
    return 0;
}

} // namespace

int wmain(const int argc, wchar_t* argv[]) {
    const auto parsed = ParseArguments(argc, argv);
    if (!parsed.options) {
        std::cerr << "Error: " << parsed.error << "\n\n";
        PrintUsage();
        return 2;
    }
    const auto& options = *parsed.options;
    if (options.help) {
        PrintUsage();
        return 0;
    }
    if (options.version) {
        if (options.json) {
            const nlohmann::json output{
                {"schema_version", 1},
                {"application", "NativeShift"},
                {"version", NATIVESHIFT_VERSION},
            };
            std::cout << output.dump() << '\n';
        } else {
            std::cout << "NativeShift " << NATIVESHIFT_VERSION << '\n';
        }
        return 0;
    }
    if (options.list_formats) {
        PrintFormats(options.json);
        return 0;
    }
    if (options.list_codecs) {
        PrintCapabilities(options.json, true, false);
        return 0;
    }
    if (options.list_hardware) {
        PrintCapabilities(options.json, false, true);
        return 0;
    }
    if (options.list_presets) {
        PrintPresets(options.json);
        return 0;
    }
    if (options.capabilities) {
        PrintCapabilities(options.json);
        return 0;
    }

    std::string error;
    const auto inputs = CollectInputs(options, error);
    if (!error.empty()) {
        std::cerr << "Error: " << error << '\n';
        return 3;
    }
    auto requests = BuildRequests(options, inputs, error);
    if (!requests) {
        std::cerr << "Error: " << error << '\n';
        return 3;
    }

    std::signal(SIGINT, HandleInterrupt);
    nativeshift::core::Logger logger(
        nativeshift::core::Logger::DefaultLogPath(), options.verbose,
        options.verbose ? nativeshift::core::LogLevel::Debug
        : options.quiet ? nativeshift::core::LogLevel::Error
                        : nativeshift::core::LogLevel::Information);
    nativeshift::platform::WindowsPlatformServices platform;
    nativeshift::core::ConversionEngine engine(&logger, &platform);
    engine.RegisterProvider(
        std::make_shared<nativeshift::image::ImageConversionProvider>());
    engine.RegisterProvider(
        std::make_shared<nativeshift::media::MediaConversionProvider>());
    nativeshift::core::JobQueue queue(engine, options.jobs, 64);

    std::vector<nativeshift::core::JobHandle> active;
    std::vector<ConversionResult> results;
    results.reserve(requests->size());
    std::size_t next = 0;
    bool cancellation_sent = false;

    while (next < requests->size() || !active.empty()) {
        while (!interrupted.load(std::memory_order_relaxed) &&
               next < requests->size() && active.size() < options.jobs + 64) {
            auto input_name =
                PathToUtf8((*requests)[next].input_path.filename());
            auto progress =
                options.verbose && !options.json && !options.quiet
                    ? nativeshift::core::ProgressCallback(
                          [input_name = std::move(input_name)](
                              const nativeshift::core::ConversionProgress&
                                  update) {
                              std::cerr
                                  << "[progress] " << input_name << ' '
                                  << static_cast<int>(
                                         std::clamp(update.fraction, 0.0, 1.0) *
                                         100.0)
                                  << "% " << update.stage << '\n';
                          })
                    : nativeshift::core::ProgressCallback{};
            auto handle = queue.TrySubmit(std::move((*requests)[next]),
                                          std::move(progress));
            if (!handle) {
                break;
            }
            active.push_back(std::move(*handle));
            ++next;
        }

        if (interrupted.load(std::memory_order_relaxed) && !cancellation_sent) {
            queue.CancelAll();
            cancellation_sent = true;
        }

        bool completed_any = false;
        for (auto iterator = active.begin(); iterator != active.end();) {
            if (!iterator->IsReady()) {
                ++iterator;
                continue;
            }
            auto result = iterator->Get();
            if (!options.json && (!options.quiet ||
                                  result.status != ConversionStatus::Success)) {
                std::cout << '[' << nativeshift::core::ToString(result.status)
                          << "] " << result.message;
                if (!result.output_path.empty()) {
                    std::cout << " -> " << PathToUtf8(result.output_path);
                }
                std::cout << '\n';
                for (const auto& warning : result.warnings) {
                    std::cout << "  warning: " << warning << '\n';
                }
            }
            results.push_back(std::move(result));
            iterator = active.erase(iterator);
            completed_any = true;
        }

        if (cancellation_sent && active.empty()) {
            break;
        }
        if (!completed_any) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    if (cancellation_sent) {
        while (next < requests->size()) {
            ConversionResult result;
            result.status = ConversionStatus::Cancelled;
            result.error = nativeshift::core::ErrorCategory::Cancelled;
            result.message = "The job was cancelled before it was submitted.";
            results.push_back(std::move(result));
            ++next;
        }
    }

    if (options.json) {
        nlohmann::json output;
        output["schema_version"] = 1;
        output["application"] = "NativeShift";
        output["version"] = NATIVESHIFT_VERSION;
        output["results"] = nlohmann::json::array();
        for (const auto& result : results) {
            output["results"].push_back(ResultJson(result));
        }
        std::cout << output.dump() << '\n';
    }
    return ExitCodeFor(results);
}
