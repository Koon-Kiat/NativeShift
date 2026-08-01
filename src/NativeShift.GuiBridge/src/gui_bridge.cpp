#include "nativeshift/gui_bridge.h"

#include "nativeshift/core/conversion_engine.hpp"
#include "nativeshift/core/format_detector.hpp"
#include "nativeshift/core/job_queue.hpp"
#include "nativeshift/core/logger.hpp"
#include "nativeshift/core/presets.hpp"
#include "nativeshift/core/settings.hpp"
#include "nativeshift/image/image_provider.hpp"
#include "nativeshift/media/media_capabilities.hpp"
#include "nativeshift/media/media_provider.hpp"
#include "nativeshift/platform/windows_platform_services.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

using nativeshift::core::ConversionRequest;
using nativeshift::core::FileFormat;
using nativeshift::core::JobHandle;

std::string PathToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string WideToUtf8(const std::wstring_view value) {
#ifdef _WIN32
    if (value.empty()) {
        return {};
    }
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    (void)::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                static_cast<int>(value.size()), result.data(),
                                required, nullptr, nullptr);
    return result;
#else
    return std::string(value.begin(), value.end());
#endif
}

std::wstring Utf8ToWide(const std::string_view value) {
#ifdef _WIN32
    if (value.empty()) {
        return {};
    }
    const int required =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                              static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    (void)::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                static_cast<int>(value.size()), result.data(),
                                required);
    return result;
#else
    return std::wstring(value.begin(), value.end());
#endif
}

std::size_t CopyResult(const std::wstring_view value, wchar_t* destination,
                       const std::size_t destination_size) {
    const auto required = value.size() + 1;
    if (destination == nullptr || destination_size < required) {
        return required;
    }
    std::ranges::copy(value, destination);
    destination[value.size()] = L'\0';
    return required;
}

void ApplyOptions(ConversionRequest& request, const wchar_t* options_json) {
    if (options_json == nullptr || *options_json == L'\0') {
        return;
    }
    const auto document = nlohmann::json::parse(WideToUtf8(options_json));

    const auto preset_id = document.value("preset_id", std::string{});
    if (!preset_id.empty()) {
        auto presets = nativeshift::core::PresetStore::BuiltIns();
        const auto custom = nativeshift::core::PresetStore().LoadCustom();
        presets.insert(presets.end(), custom.presets.begin(),
                       custom.presets.end());
        const auto preset = std::ranges::find(
            presets, preset_id, &nativeshift::core::ConversionPreset::id);
        if (preset == presets.end()) {
            throw std::runtime_error("The selected preset was not found.");
        }
        if (preset->output_format != request.output_format) {
            throw std::runtime_error(
                "The selected preset does not match the output format.");
        }
        request.image = preset->image;
        request.audio = preset->audio;
        request.video = preset->video;
    }

    request.image.quality =
        document.value("image_quality", request.image.quality);
    request.image.preserve_metadata =
        document.value("preserve_metadata", false);
    request.audio.preserve_metadata =
        document.value("preserve_metadata", false);
    request.video.preserve_metadata =
        document.value("preserve_metadata", false);
    request.audio.bitrate_kbps =
        document.value("audio_bitrate_kbps", request.audio.bitrate_kbps);
    request.video.audio_bitrate_kbps = document.value(
        "video_audio_bitrate_kbps", request.video.audio_bitrate_kbps);
    if (document.contains("video_quality") &&
        document["video_quality"].is_number_integer()) {
        request.video.quality = document["video_quality"].get<int>();
    }
    const auto audio_codec =
        document.value("audio_codec", std::string{"automatic"});
    const std::map<std::string, nativeshift::core::AudioCodec> audio_codecs{
        {"automatic", nativeshift::core::AudioCodec::Automatic},
        {"mp3", nativeshift::core::AudioCodec::Mp3},
        {"pcm", nativeshift::core::AudioCodec::PcmS16},
        {"flac", nativeshift::core::AudioCodec::Flac},
        {"aac", nativeshift::core::AudioCodec::Aac},
        {"vorbis", nativeshift::core::AudioCodec::Vorbis},
        {"opus", nativeshift::core::AudioCodec::Opus},
        {"copy", nativeshift::core::AudioCodec::Copy},
    };
    if (const auto found = audio_codecs.find(audio_codec);
        found != audio_codecs.end()) {
        request.audio.codec = found->second;
    }
    const auto video_codec =
        document.value("video_codec", std::string{"automatic"});
    const std::map<std::string, nativeshift::core::VideoCodec> video_codecs{
        {"automatic", nativeshift::core::VideoCodec::Automatic},
        {"h264", nativeshift::core::VideoCodec::H264},
        {"h265", nativeshift::core::VideoCodec::H265},
        {"vp9", nativeshift::core::VideoCodec::Vp9},
        {"av1", nativeshift::core::VideoCodec::Av1},
        {"copy", nativeshift::core::VideoCodec::Copy},
    };
    if (const auto found = video_codecs.find(video_codec);
        found != video_codecs.end()) {
        request.video.video_codec = found->second;
    }
    if (document.contains("width") && document["width"].is_number_unsigned()) {
        const auto width = document["width"].get<std::uint32_t>();
        request.image.width = width;
        request.video.width = width;
    }
    if (document.contains("height") &&
        document["height"].is_number_unsigned()) {
        const auto height = document["height"].get<std::uint32_t>();
        request.image.height = height;
        request.video.height = height;
    }
    const auto hardware = document.value("hardware", std::string{"auto"});
    if (hardware == "software") {
        request.video.hardware_acceleration =
            nativeshift::core::HardwareAcceleration::SoftwareOnly;
    } else if (hardware == "prefer") {
        request.video.hardware_acceleration =
            nativeshift::core::HardwareAcceleration::PreferHardware;
    }
    if (const auto policy = nativeshift::core::OutputConflictPolicyFromString(
            document.value("conflict", std::string{"unique"}));
        policy.has_value()) {
        request.conflict_policy = *policy;
    }
}

nlohmann::json JobJson(const JobHandle& handle) {
    const auto job = handle.Snapshot();
    nlohmann::json value{
        {"id", job.id},
        {"state", nativeshift::core::ToString(job.state)},
        {"progress", std::clamp(job.progress.fraction, 0.0, 1.0)},
        {"stage", job.progress.stage},
        {"input_name", PathToUtf8(job.request.input_path.filename())},
        {"input_path", PathToUtf8(job.request.input_path)},
        {"output_path", PathToUtf8(job.request.output_path)},
        {"input_format", nativeshift::core::ToString(job.request.input_format)},
        {"output_format",
         nativeshift::core::ToString(job.request.output_format)},
    };
    if (job.result.has_value()) {
        value["status"] = nativeshift::core::ToString(job.result->status);
        value["message"] = job.result->message;
        value["provider"] = job.result->provider;
        value["selected_codec"] = job.result->selected_codec;
        value["hardware_acceleration"] = job.result->hardware_acceleration;
        value["duration_ms"] = job.result->duration.count();
        value["warnings"] = job.result->warnings;
    }
    return value;
}

class BridgeService {
  public:
    BridgeService()
        : BridgeService(nativeshift::core::SettingsStore().Load().settings) {}

  private:
    explicit BridgeService(const nativeshift::core::UserSettings& settings)
        : logger_(nativeshift::core::Logger::DefaultLogPath(),
                  settings.logging_level == nativeshift::core::LogLevel::Debug,
                  settings.logging_level),
          engine_(&logger_, &platform_),
          queue_(engine_, settings.maximum_concurrent_conversions, 256) {
        engine_.RegisterProvider(
            std::make_shared<nativeshift::image::ImageConversionProvider>());
        engine_.RegisterProvider(
            std::make_shared<nativeshift::media::MediaConversionProvider>());
    }

  public:
    std::uint64_t Submit(const wchar_t* input_path, const wchar_t* output_path,
                         const wchar_t* output_format,
                         const wchar_t* options_json) {
        try {
            if (input_path == nullptr || output_path == nullptr ||
                output_format == nullptr) {
                SetError("Input, output, and format are required.");
                return 0;
            }
            const auto format =
                nativeshift::core::FormatFromString(WideToUtf8(output_format));
            if (!format.has_value() || *format == FileFormat::Unknown ||
                *format == FileFormat::Mov || *format == FileFormat::Avi) {
                SetError("The requested output format is unsupported.");
                return 0;
            }
            ConversionRequest request;
            request.input_path = input_path;
            request.output_path = output_path;
            request.input_format =
                nativeshift::core::DetectFormat(request.input_path).format;
            request.output_format = *format;
            request.conflict_policy =
                nativeshift::core::OutputConflictPolicy::GenerateUniqueName;
            ApplyOptions(request, options_json);
            auto handle = queue_.TrySubmit(std::move(request));
            if (!handle.has_value()) {
                SetError("The conversion queue is full.");
                return 0;
            }
            const auto id = handle->Id();
            {
                std::scoped_lock lock(mutex_);
                jobs_.insert_or_assign(id, std::move(*handle));
                last_error_.clear();
            }
            return id;
        } catch (const std::exception& exception) {
            SetError(exception.what());
            return 0;
        }
    }

    bool Cancel(const std::uint64_t id) {
        std::scoped_lock lock(mutex_);
        const auto found = jobs_.find(id);
        if (found == jobs_.end()) {
            return false;
        }
        found->second.Cancel();
        return true;
    }

    std::uint64_t Retry(const std::uint64_t id) {
        std::scoped_lock lock(mutex_);
        const auto found = jobs_.find(id);
        if (found == jobs_.end()) {
            last_error_ = "The selected job no longer exists.";
            return 0;
        }
        auto handle = queue_.Requeue(found->second);
        if (!handle.has_value()) {
            last_error_ = "Only a completed, cancelled, or failed job can be "
                          "retried.";
            return 0;
        }
        const auto new_id = handle->Id();
        jobs_.insert_or_assign(new_id, std::move(*handle));
        last_error_.clear();
        return new_id;
    }

    void CancelAll() { queue_.CancelAll(); }
    void Pause() { queue_.Pause(); }
    void Resume() { queue_.Resume(); }

    bool Remove(const std::uint64_t id) {
        std::scoped_lock lock(mutex_);
        const auto found = jobs_.find(id);
        if (found == jobs_.end() || !found->second.IsReady()) {
            return false;
        }
        jobs_.erase(found);
        return true;
    }

    std::wstring Job(const std::uint64_t id) const {
        std::scoped_lock lock(mutex_);
        const auto found = jobs_.find(id);
        if (found == jobs_.end()) {
            return Utf8ToWide(
                nlohmann::json{{"error", "job_not_found"}}.dump());
        }
        return Utf8ToWide(JobJson(found->second).dump());
    }

    std::wstring Queue() const {
        std::scoped_lock lock(mutex_);
        nlohmann::json document{
            {"schema_version", 1},
            {"paused", queue_.IsPaused()},
            {"jobs", nlohmann::json::array()},
        };
        for (const auto& [id, handle] : jobs_) {
            (void)id;
            document["jobs"].push_back(JobJson(handle));
        }
        return Utf8ToWide(document.dump());
    }

    std::wstring Detect(const wchar_t* input_path) const {
        if (input_path == nullptr || *input_path == L'\0') {
            return L"unknown";
        }
        return Utf8ToWide(nativeshift::core::ToString(
            nativeshift::core::DetectFormat(input_path).format));
    }

    std::wstring Capabilities() const {
        const auto& detected = nativeshift::media::DetectMediaCapabilities();
        nlohmann::json document{
            {"schema_version", 1},
            {"ffmpeg_version", detected.ffmpeg_version},
            {"hardware_devices", nlohmann::json::array()},
            {"encoders", nlohmann::json::array()},
        };
        for (const auto& device : detected.hardware_devices) {
            document["hardware_devices"].push_back(
                {{"name", device.name},
                 {"available", device.available},
                 {"diagnostic", device.diagnostic}});
        }
        for (const auto& encoder : detected.encoders) {
            document["encoders"].push_back({{"name", encoder.name},
                                            {"codec", encoder.codec},
                                            {"registered", encoder.registered},
                                            {"hardware", encoder.hardware}});
        }
        return Utf8ToWide(document.dump());
    }

    std::wstring Presets() const {
        nlohmann::json document{
            {"schema_version", 1},
            {"presets", nlohmann::json::array()},
        };
        auto presets = nativeshift::core::PresetStore::BuiltIns();
        const auto custom = nativeshift::core::PresetStore().LoadCustom();
        presets.insert(presets.end(), custom.presets.begin(),
                       custom.presets.end());
        for (const auto& preset : presets) {
            document["presets"].push_back(
                {{"id", preset.id},
                 {"name", preset.name},
                 {"output_format",
                  nativeshift::core::ToString(preset.output_format)},
                 {"built_in", preset.built_in}});
        }
        return Utf8ToWide(document.dump());
    }

    std::wstring Settings() const {
        const auto loaded = nativeshift::core::SettingsStore().Load();
        const auto& settings = loaded.settings;
        const auto theme =
            settings.theme == nativeshift::core::ThemePreference::Light
                ? "light"
            : settings.theme == nativeshift::core::ThemePreference::Dark
                ? "dark"
                : "system";
        const auto level =
            settings.logging_level == nativeshift::core::LogLevel::Debug
                ? "debug"
            : settings.logging_level == nativeshift::core::LogLevel::Warning
                ? "warning"
            : settings.logging_level == nativeshift::core::LogLevel::Error
                ? "error"
                : "information";
        const auto hardware =
            settings.hardware_acceleration ==
                    nativeshift::core::HardwareAccelerationPreference::
                        PreferHardware
                ? "prefer_hardware"
            : settings.hardware_acceleration ==
                    nativeshift::core::HardwareAccelerationPreference::Disabled
                ? "disabled"
                : "auto";
        const nlohmann::json document{
            {"schema_version", 1},
            {"settings_version", settings.version},
            {"default_output_folder",
             PathToUtf8(settings.default_output_folder)},
            {"maximum_concurrent_conversions",
             settings.maximum_concurrent_conversions},
            {"hardware_acceleration", hardware},
            {"preserve_metadata", settings.preserve_metadata},
            {"existing_file_policy",
             nativeshift::core::ToString(settings.existing_file_policy)},
            {"theme", theme},
            {"logging_level", level},
            {"notifications_enabled", settings.notifications_enabled},
            {"warning", loaded.warning},
        };
        return Utf8ToWide(document.dump());
    }

    bool SaveSettings(const wchar_t* settings_json) {
        try {
            if (settings_json == nullptr) {
                SetError("Settings JSON is required.");
                return false;
            }
            auto store = nativeshift::core::SettingsStore();
            auto settings = store.Load().settings;
            const auto document =
                nlohmann::json::parse(WideToUtf8(settings_json));
            settings.default_output_folder = std::filesystem::path(Utf8ToWide(
                document.value("default_output_folder",
                               PathToUtf8(settings.default_output_folder))));
            settings.maximum_concurrent_conversions =
                document.value("maximum_concurrent_conversions",
                               settings.maximum_concurrent_conversions);
            const auto hardware =
                document.value("hardware_acceleration", std::string{"auto"});
            settings.hardware_acceleration =
                hardware == "prefer_hardware"
                    ? nativeshift::core::HardwareAccelerationPreference::
                          PreferHardware
                : hardware == "disabled"
                    ? nativeshift::core::HardwareAccelerationPreference::
                          Disabled
                    : nativeshift::core::HardwareAccelerationPreference::Auto;
            settings.preserve_metadata =
                document.value("preserve_metadata", settings.preserve_metadata);
            if (const auto policy =
                    nativeshift::core::OutputConflictPolicyFromString(
                        document.value("existing_file_policy",
                                       std::string(nativeshift::core::ToString(
                                           settings.existing_file_policy))));
                policy.has_value()) {
                settings.existing_file_policy = *policy;
            }
            const auto theme = document.value("theme", std::string{"system"});
            settings.theme =
                theme == "light"  ? nativeshift::core::ThemePreference::Light
                : theme == "dark" ? nativeshift::core::ThemePreference::Dark
                                  : nativeshift::core::ThemePreference::System;
            const auto level =
                document.value("logging_level", std::string{"information"});
            settings.logging_level =
                level == "debug"     ? nativeshift::core::LogLevel::Debug
                : level == "warning" ? nativeshift::core::LogLevel::Warning
                : level == "error"   ? nativeshift::core::LogLevel::Error
                                     : nativeshift::core::LogLevel::Information;
            settings.notifications_enabled = document.value(
                "notifications_enabled", settings.notifications_enabled);
            std::string error;
            if (!store.Save(settings, error)) {
                SetError(error);
                return false;
            }
            SetError({});
            return true;
        } catch (const std::exception& exception) {
            SetError(exception.what());
            return false;
        }
    }

    bool ResetSettings() {
        std::string error;
        if (!nativeshift::core::SettingsStore().Reset(error)) {
            SetError(error);
            return false;
        }
        SetError({});
        return true;
    }

    std::wstring DiagnosticSummary() const {
        return Utf8ToWide(logger_.DiagnosticSummary(
            nativeshift::media::DetectMediaCapabilities().ffmpeg_version));
    }

    std::wstring LogFolder() const {
        return logger_.Path().parent_path().wstring();
    }

    std::wstring LastError() const {
        std::scoped_lock lock(mutex_);
        return Utf8ToWide(last_error_);
    }

  private:
    void SetError(std::string value) {
        std::scoped_lock lock(mutex_);
        last_error_ = std::move(value);
    }

    nativeshift::core::Logger logger_;
    nativeshift::platform::WindowsPlatformServices platform_;
    nativeshift::core::ConversionEngine engine_;
    nativeshift::core::JobQueue queue_;
    mutable std::mutex mutex_;
    std::map<std::uint64_t, JobHandle> jobs_;
    std::string last_error_;
};

BridgeService& Service() {
    static BridgeService service;
    return service;
}

template <typename Provider>
std::size_t WriteJson(Provider&& provider, wchar_t* destination,
                      const std::size_t destination_size) {
    try {
        return CopyResult(provider(), destination, destination_size);
    } catch (...) {
        return CopyResult(LR"({"error":"bridge_failure"})", destination,
                          destination_size);
    }
}

} // namespace

extern "C" {

std::uint64_t nativeshift_submit(const wchar_t* input_path,
                                 const wchar_t* output_path,
                                 const wchar_t* output_format,
                                 const wchar_t* options_json) {
    return Service().Submit(input_path, output_path, output_format,
                            options_json);
}

bool nativeshift_cancel(const std::uint64_t job_id) {
    return Service().Cancel(job_id);
}

std::uint64_t nativeshift_retry(const std::uint64_t job_id) {
    return Service().Retry(job_id);
}

void nativeshift_cancel_all() { Service().CancelAll(); }

void nativeshift_pause_queue() { Service().Pause(); }

void nativeshift_resume_queue() { Service().Resume(); }

bool nativeshift_remove(const std::uint64_t job_id) {
    return Service().Remove(job_id);
}

std::size_t nativeshift_job_json(const std::uint64_t job_id,
                                 wchar_t* destination,
                                 const std::size_t destination_size) {
    return WriteJson([job_id] { return Service().Job(job_id); }, destination,
                     destination_size);
}

std::size_t nativeshift_queue_json(wchar_t* destination,
                                   const std::size_t destination_size) {
    return WriteJson([] { return Service().Queue(); }, destination,
                     destination_size);
}

std::size_t nativeshift_detect_format(const wchar_t* input_path,
                                      wchar_t* destination,
                                      const std::size_t destination_size) {
    return WriteJson([input_path] { return Service().Detect(input_path); },
                     destination, destination_size);
}

std::size_t nativeshift_capabilities_json(wchar_t* destination,
                                          const std::size_t destination_size) {
    return WriteJson([] { return Service().Capabilities(); }, destination,
                     destination_size);
}

std::size_t nativeshift_presets_json(wchar_t* destination,
                                     const std::size_t destination_size) {
    return WriteJson([] { return Service().Presets(); }, destination,
                     destination_size);
}

std::size_t nativeshift_settings_json(wchar_t* destination,
                                      const std::size_t destination_size) {
    return WriteJson([] { return Service().Settings(); }, destination,
                     destination_size);
}

bool nativeshift_save_settings(const wchar_t* settings_json) {
    return Service().SaveSettings(settings_json);
}

bool nativeshift_reset_settings() { return Service().ResetSettings(); }

std::size_t nativeshift_diagnostic_summary(wchar_t* destination,
                                           const std::size_t destination_size) {
    return WriteJson([] { return Service().DiagnosticSummary(); }, destination,
                     destination_size);
}

std::size_t nativeshift_log_folder(wchar_t* destination,
                                   const std::size_t destination_size) {
    return WriteJson([] { return Service().LogFolder(); }, destination,
                     destination_size);
}

std::size_t nativeshift_last_error(wchar_t* destination,
                                   const std::size_t destination_size) {
    return WriteJson([] { return Service().LastError(); }, destination,
                     destination_size);
}
}
