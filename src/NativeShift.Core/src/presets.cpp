#include "nativeshift/core/presets.hpp"

#include "nativeshift/core/output_paths.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_set>

namespace nativeshift::core {
namespace {

std::string MakeId(const std::string& name) {
    std::string id;
    id.reserve(name.size());
    bool separator = false;
    for (const auto character : name) {
        const auto value = static_cast<unsigned char>(character);
        if (std::isalnum(value) != 0) {
            id.push_back(static_cast<char>(std::tolower(value)));
            separator = false;
        } else if (!id.empty() && !separator) {
            id.push_back('-');
            separator = true;
        }
    }
    while (!id.empty() && id.back() == '-') {
        id.pop_back();
    }
    return id;
}

template <typename Value>
nlohmann::json JsonOptional(const std::optional<Value>& value) {
    return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

template <typename Value>
std::optional<Value> OptionalValue(const nlohmann::json& value,
                                   const char* key) {
    if (!value.contains(key) || value[key].is_null() ||
        !value[key].is_number()) {
        return std::nullopt;
    }
    return value[key].get<Value>();
}

nlohmann::json Serialize(const ConversionPreset& preset) {
    return {
        {"id", preset.id},
        {"name", preset.name},
        {"media_kind", static_cast<int>(preset.media_kind)},
        {"output_format", ToString(preset.output_format)},
        {"image",
         {{"quality", preset.image.quality},
          {"compression_level", preset.image.compression_level},
          {"lossless", preset.image.lossless}}},
        {"audio",
         {{"codec", static_cast<int>(preset.audio.codec)},
          {"bitrate_kbps", preset.audio.bitrate_kbps},
          {"variable_bitrate", preset.audio.variable_bitrate},
          {"sample_rate", JsonOptional(preset.audio.sample_rate)},
          {"channels", JsonOptional(preset.audio.channels)}}},
        {"video",
         {{"video_codec", static_cast<int>(preset.video.video_codec)},
          {"audio_codec", static_cast<int>(preset.video.audio_codec)},
          {"quality", JsonOptional(preset.video.quality)},
          {"video_bitrate_kbps", JsonOptional(preset.video.video_bitrate_kbps)},
          {"width", JsonOptional(preset.video.width)},
          {"height", JsonOptional(preset.video.height)},
          {"encoder_preset", preset.video.encoder_preset}}},
    };
}

template <typename Enum>
Enum EnumValue(const nlohmann::json& value, const char* key,
               const Enum fallback, const int maximum) {
    const auto raw = value.value(key, static_cast<int>(fallback));
    if (raw < 0 || raw > maximum) {
        return fallback;
    }
    return static_cast<Enum>(raw);
}

std::optional<ConversionPreset> Deserialize(const nlohmann::json& value,
                                            const bool legacy) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    ConversionPreset preset;
    preset.name = value.value("name", "");
    preset.id = value.value("id", MakeId(preset.name));
    if (preset.name.empty() || preset.id.empty()) {
        return std::nullopt;
    }

    const auto output =
        FormatFromString(value.value(legacy ? "format" : "output_format", ""));
    if (!output.has_value()) {
        return std::nullopt;
    }
    preset.output_format = *output;
    preset.media_kind = KindOf(*output);
    preset.built_in = false;

    if (value.contains("image") && value["image"].is_object()) {
        const auto& image = value["image"];
        preset.image.quality = image.value("quality", preset.image.quality);
        preset.image.compression_level =
            image.value("compression_level", preset.image.compression_level);
        preset.image.lossless = image.value("lossless", false);
    } else if (legacy) {
        preset.image.quality = value.value("quality", preset.image.quality);
    }
    if (value.contains("audio") && value["audio"].is_object()) {
        const auto& audio = value["audio"];
        preset.audio.codec = EnumValue(audio, "codec", AudioCodec::Automatic,
                                       static_cast<int>(AudioCodec::Copy));
        preset.audio.bitrate_kbps =
            audio.value("bitrate_kbps", preset.audio.bitrate_kbps);
        preset.audio.variable_bitrate = audio.value("variable_bitrate", false);
        preset.audio.sample_rate = OptionalValue<int>(audio, "sample_rate");
        preset.audio.channels = OptionalValue<int>(audio, "channels");
    }
    if (value.contains("video") && value["video"].is_object()) {
        const auto& video = value["video"];
        preset.video.video_codec =
            EnumValue(video, "video_codec", VideoCodec::Automatic,
                      static_cast<int>(VideoCodec::Copy));
        preset.video.audio_codec =
            EnumValue(video, "audio_codec", VideoAudioCodec::Automatic,
                      static_cast<int>(VideoAudioCodec::None));
        preset.video.quality = OptionalValue<int>(video, "quality");
        preset.video.video_bitrate_kbps =
            OptionalValue<int>(video, "video_bitrate_kbps");
        preset.video.width = OptionalValue<std::uint32_t>(video, "width");
        preset.video.height = OptionalValue<std::uint32_t>(video, "height");
        preset.video.encoder_preset = video.value("encoder_preset", "medium");
    }
    return preset;
}

ConversionPreset ImagePreset(std::string id, std::string name,
                             const FileFormat output, const int quality,
                             const int compression, const bool lossless) {
    ConversionPreset preset;
    preset.id = std::move(id);
    preset.name = std::move(name);
    preset.media_kind = MediaKind::Image;
    preset.output_format = output;
    preset.image.quality = quality;
    preset.image.compression_level = compression;
    preset.image.lossless = lossless;
    preset.built_in = true;
    return preset;
}

ConversionPreset AudioPreset(std::string id, std::string name,
                             const FileFormat output, const AudioCodec codec,
                             const int bitrate, const bool variable) {
    ConversionPreset preset;
    preset.id = std::move(id);
    preset.name = std::move(name);
    preset.media_kind = MediaKind::Audio;
    preset.output_format = output;
    preset.audio.codec = codec;
    preset.audio.bitrate_kbps = bitrate;
    preset.audio.variable_bitrate = variable;
    preset.audio.preserve_source_settings = false;
    preset.built_in = true;
    return preset;
}

ConversionPreset VideoPreset(std::string id, std::string name,
                             const FileFormat output, const VideoCodec codec,
                             const std::optional<std::uint32_t> height = {}) {
    ConversionPreset preset;
    preset.id = std::move(id);
    preset.name = std::move(name);
    preset.media_kind = MediaKind::Video;
    preset.output_format = output;
    preset.video.video_codec = codec;
    preset.video.audio_codec = output == FileFormat::WebM
                                   ? VideoAudioCodec::Opus
                                   : VideoAudioCodec::Aac;
    preset.video.height = height;
    preset.video.quality = 23;
    preset.built_in = true;
    return preset;
}

} // namespace

PresetStore::PresetStore(std::filesystem::path path) : path_(std::move(path)) {}

PresetLoadResult PresetStore::LoadCustom() const {
    PresetLoadResult result;
    std::error_code error;
    if (!std::filesystem::exists(path_, error)) {
        if (error) {
            result.recovered_from_error = true;
            result.warning =
                "Custom presets could not be inspected; none were loaded.";
        }
        return result;
    }

    try {
        std::ifstream input(path_);
        nlohmann::json document;
        input >> document;
        const auto version = document.value("version", 0);
        if (version < 0 || version > kCurrentVersion) {
            result.recovered_from_error = true;
            result.warning =
                "The custom preset schema is unsupported; none were loaded.";
            return result;
        }
        result.migrated = version < kCurrentVersion;
        const auto values = document.contains("presets")
                                ? document["presets"]
                                : nlohmann::json::array();
        if (!values.is_array()) {
            throw std::runtime_error("presets must be an array");
        }
        std::unordered_set<std::string> ids;
        for (const auto& value : values) {
            auto preset = Deserialize(value, version == 0);
            if (preset.has_value() && ids.insert(preset->id).second) {
                result.presets.push_back(std::move(*preset));
            }
        }
        if (result.migrated) {
            auto backup = path_;
            backup += L".v" + std::to_wstring(version) + L".bak";
            std::filesystem::copy_file(
                path_, backup,
                std::filesystem::copy_options::overwrite_existing, error);
            std::string save_error;
            if (error || !SaveCustom(result.presets, save_error)) {
                result.warning =
                    "Presets were loaded but could not be migrated safely.";
            }
        }
    } catch (const std::exception&) {
        result.presets.clear();
        result.recovered_from_error = true;
        result.warning = "Custom presets were corrupt; none were loaded.";
    }
    return result;
}

bool PresetStore::SaveCustom(const std::vector<ConversionPreset>& presets,
                             std::string& error) const {
    nlohmann::json values = nlohmann::json::array();
    std::unordered_set<std::string> ids;
    for (const auto& preset : presets) {
        if (preset.built_in || preset.name.empty() || preset.id.empty() ||
            !ids.insert(preset.id).second) {
            error =
                "Custom presets must have unique IDs and cannot be built-in.";
            return false;
        }
        const auto issues = Validate(preset, preset.output_format);
        if (!issues.empty()) {
            error = issues.front().message;
            return false;
        }
        values.push_back(Serialize(preset));
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(path_.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "The preset directory could not be created.";
        return false;
    }
    const auto temporary = MakeTemporaryOutputPath(path_);
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            error = "The temporary preset file could not be opened.";
            return false;
        }
        output << nlohmann::json{{"version", kCurrentVersion},
                                 {"presets", values}}
                      .dump(2)
               << '\n';
        if (!output) {
            std::filesystem::remove(temporary, filesystem_error);
            error = "The temporary preset file could not be written.";
            return false;
        }
    }
    if (!CommitTemporaryOutput(temporary, path_, true, error)) {
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    return true;
}

const std::filesystem::path& PresetStore::Path() const noexcept {
    return path_;
}

std::vector<ConversionPreset> PresetStore::BuiltIns() {
    return {
        ImagePreset("jpeg-high", "JPEG high quality", FileFormat::Jpeg, 95, 6,
                    false),
        ImagePreset("jpeg-balanced", "JPEG balanced", FileFormat::Jpeg, 85, 6,
                    false),
        ImagePreset("webp-lossless", "WebP lossless", FileFormat::WebP, 100, 6,
                    true),
        ImagePreset("webp-balanced", "WebP balanced", FileFormat::WebP, 82, 6,
                    false),
        ImagePreset("png-optimized", "PNG optimized", FileFormat::Png, 100, 9,
                    true),
        AudioPreset("mp3-high", "MP3 high quality", FileFormat::Mp3,
                    AudioCodec::Mp3, 320, false),
        AudioPreset("mp3-compact", "MP3 compact", FileFormat::Mp3,
                    AudioCodec::Mp3, 128, false),
        AudioPreset("flac-lossless", "FLAC lossless", FileFormat::Flac,
                    AudioCodec::Flac, 0, false),
        AudioPreset("aac-balanced", "AAC balanced", FileFormat::Aac,
                    AudioCodec::Aac, 192, false),
        AudioPreset("opus-voice", "Opus voice", FileFormat::Opus,
                    AudioCodec::Opus, 48, true),
        AudioPreset("opus-music", "Opus music", FileFormat::Opus,
                    AudioCodec::Opus, 160, true),
        VideoPreset("mp4-h264", "MP4 H.264 compatibility", FileFormat::Mp4,
                    VideoCodec::H264),
        VideoPreset("mp4-h265", "MP4 H.265 smaller size", FileFormat::Mp4,
                    VideoCodec::H265),
        VideoPreset("webm-vp9", "WebM VP9", FileFormat::WebM, VideoCodec::Vp9),
        VideoPreset("webm-av1", "WebM AV1", FileFormat::WebM, VideoCodec::Av1),
        VideoPreset("video-source", "Preserve source resolution",
                    FileFormat::Mp4, VideoCodec::H264),
        VideoPreset("video-1080p", "1080p", FileFormat::Mp4, VideoCodec::H264,
                    1080),
        VideoPreset("video-720p", "720p", FileFormat::Mp4, VideoCodec::H264,
                    720),
    };
}

std::optional<ConversionPreset>
PresetStore::Duplicate(const ConversionPreset& source, std::string name) {
    if (name.empty()) {
        return std::nullopt;
    }
    auto copy = source;
    copy.name = std::move(name);
    copy.id = MakeId(copy.name);
    copy.built_in = false;
    if (copy.id.empty()) {
        return std::nullopt;
    }
    return copy;
}

bool PresetStore::Rename(ConversionPreset& preset, std::string name) {
    if (preset.built_in || name.empty()) {
        return false;
    }
    preset.name = std::move(name);
    preset.id = MakeId(preset.name);
    return !preset.id.empty();
}

bool PresetStore::Delete(std::vector<ConversionPreset>& presets,
                         const std::string_view id) {
    const auto found = std::ranges::find_if(presets, [id](const auto& preset) {
        return !preset.built_in && preset.id == id;
    });
    if (found == presets.end()) {
        return false;
    }
    presets.erase(found);
    return true;
}

std::vector<ValidationIssue>
PresetStore::Validate(const ConversionPreset& preset,
                      const FileFormat input_format) {
    std::vector<ValidationIssue> issues;
    if (preset.name.empty() || preset.id.empty()) {
        issues.push_back({ErrorCategory::InvalidRequest, "preset_identity",
                          "The preset requires a name and identifier."});
    }
    if (preset.output_format == FileFormat::Unknown ||
        preset.media_kind != KindOf(preset.output_format)) {
        issues.push_back(
            {ErrorCategory::InvalidRequest, "preset_output",
             "The preset output format does not match its media category."});
    }
    if (input_format != FileFormat::Unknown &&
        KindOf(input_format) != preset.media_kind) {
        issues.push_back(
            {ErrorCategory::UnsupportedFormat, "preset_input",
             "The preset cannot be applied to this input media category."});
    }
    if (preset.media_kind == MediaKind::Image &&
        (preset.image.quality < 0 || preset.image.quality > 100)) {
        issues.push_back({ErrorCategory::InvalidRequest, "preset_quality",
                          "Image quality must be between 0 and 100."});
    }
    if (preset.media_kind == MediaKind::Audio &&
        (preset.audio.bitrate_kbps < 0 || preset.audio.bitrate_kbps > 1'536)) {
        issues.push_back({ErrorCategory::InvalidRequest, "preset_bitrate",
                          "Audio bitrate is outside the supported range."});
    }
    return issues;
}

std::filesystem::path PresetStore::DefaultPath() {
#ifdef _WIN32
    wchar_t* local_app_data = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&local_app_data, &length, L"LOCALAPPDATA") == 0 &&
        local_app_data != nullptr) {
        const auto result = std::filesystem::path(local_app_data) /
                            "NativeShift" / "presets.json";
        std::free(local_app_data);
        return result;
    }
#endif
    return std::filesystem::temp_directory_path() / "NativeShift" /
           "presets.json";
}

} // namespace nativeshift::core
