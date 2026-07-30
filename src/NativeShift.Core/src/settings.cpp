#include "nativeshift/core/settings.hpp"

#include "nativeshift/core/job_queue.hpp"
#include "nativeshift/core/output_paths.hpp"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string_view>

namespace nativeshift::core {
namespace {

std::string_view
ToString(const HardwareAccelerationPreference preference) noexcept {
    switch (preference) {
    case HardwareAccelerationPreference::PreferHardware:
        return "prefer_hardware";
    case HardwareAccelerationPreference::Disabled:
        return "disabled";
    case HardwareAccelerationPreference::Auto:
    default:
        return "auto";
    }
}

HardwareAccelerationPreference
HardwarePreferenceFromString(const std::string& value) {
    if (value == "prefer_hardware") {
        return HardwareAccelerationPreference::PreferHardware;
    }
    if (value == "disabled") {
        return HardwareAccelerationPreference::Disabled;
    }
    return HardwareAccelerationPreference::Auto;
}

std::string_view ToString(const ThemePreference preference) noexcept {
    switch (preference) {
    case ThemePreference::Light:
        return "light";
    case ThemePreference::Dark:
        return "dark";
    case ThemePreference::System:
    default:
        return "system";
    }
}

ThemePreference ThemeFromString(const std::string& value) {
    if (value == "light") {
        return ThemePreference::Light;
    }
    if (value == "dark") {
        return ThemePreference::Dark;
    }
    return ThemePreference::System;
}

UserSettings Defaults() {
    UserSettings settings;
    settings.maximum_concurrent_conversions =
        JobQueue::SafeDefaultConcurrency();
    return settings;
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path PathFromUtf8(const std::string& value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()),
        reinterpret_cast<const char8_t*>(value.data() + value.size())));
}

} // namespace

SettingsStore::SettingsStore(std::filesystem::path path,
                             std::filesystem::path legacy_path)
    : path_(std::move(path)), legacy_path_(std::move(legacy_path)) {
    if (legacy_path_.empty() && path_ == DefaultPath()) {
        legacy_path_ = LegacyPath();
    }
}

SettingsLoadResult SettingsStore::Load() const {
    SettingsLoadResult result;
    result.settings = Defaults();

    std::error_code error;
    auto source_path = path_;
    bool identity_migration = false;
    if (!std::filesystem::exists(source_path, error)) {
        if (error) {
            result.recovered_from_error = true;
            result.warning =
                "Settings could not be inspected; defaults were used.";
            return result;
        }
        error.clear();
        if (!legacy_path_.empty() &&
            std::filesystem::exists(legacy_path_, error) && !error) {
            source_path = legacy_path_;
            identity_migration = true;
        } else {
            if (error) {
                result.recovered_from_error = true;
                result.warning = "Legacy settings could not be inspected; "
                                 "defaults were used.";
            }
            return result;
        }
    }

    try {
        std::ifstream input(source_path);
        if (!input) {
            result.recovered_from_error = true;
            result.warning =
                "Settings could not be opened; defaults were used.";
            return result;
        }

        nlohmann::json json;
        input >> json;
        input.close();
        const int version = json.value("version", 0);
        if (version < 0 || version > UserSettings::kCurrentVersion) {
            result.recovered_from_error = true;
            result.warning =
                "The settings version is unsupported; defaults were used.";
            return result;
        }

        if (version == 0) {
            result.migrated = true;
            result.settings.maximum_concurrent_conversions =
                json.value("max_concurrent_jobs",
                           result.settings.maximum_concurrent_conversions);
            result.settings.preserve_metadata =
                json.value("keep_metadata", result.settings.preserve_metadata);
            if (json.contains("output_folder") &&
                json["output_folder"].is_string()) {
                result.settings.default_output_folder =
                    PathFromUtf8(json["output_folder"].get<std::string>());
            }
        } else {
            if (json.contains("default_output_folder") &&
                json["default_output_folder"].is_string()) {
                result.settings.default_output_folder = PathFromUtf8(
                    json["default_output_folder"].get<std::string>());
            }
            if (const auto format = FormatFromString(
                    json.value("preferred_image_format", "png"));
                format.has_value() && IsPhase1ImageFormat(*format)) {
                result.settings.preferred_image_format = *format;
            }
            result.settings.maximum_concurrent_conversions =
                json.value("maximum_concurrent_conversions",
                           result.settings.maximum_concurrent_conversions);
            result.settings.hardware_acceleration =
                HardwarePreferenceFromString(
                    json.value("hardware_acceleration", "auto"));
            result.settings.preserve_metadata =
                json.value("preserve_metadata", false);
            if (const auto policy = OutputConflictPolicyFromString(
                    json.value("existing_file_policy", "unique"));
                policy.has_value()) {
                result.settings.existing_file_policy = *policy;
            }
            result.settings.theme =
                ThemeFromString(json.value("theme", "system"));
        }

        result.settings.maximum_concurrent_conversions =
            std::clamp<std::size_t>(
                result.settings.maximum_concurrent_conversions, 1, 32);
        result.settings.version = UserSettings::kCurrentVersion;
        if (identity_migration) {
            result.migrated = true;
            std::string migration_error;
            if (Save(result.settings, migration_error)) {
                auto backup_path = legacy_path_;
                backup_path += L".migration.bak";
                error.clear();
                std::filesystem::copy_file(
                    legacy_path_, backup_path,
                    std::filesystem::copy_options::overwrite_existing, error);
                if (!error) {
                    std::filesystem::remove(legacy_path_, error);
                }
                result.warning =
                    "Settings were migrated from UniversalFileConverter.";
                if (error) {
                    result.warning +=
                        " The legacy file could not be archived automatically.";
                }
            } else {
                result.warning =
                    "Legacy settings were loaded but could not be migrated: " +
                    migration_error;
            }
        }
        return result;
    } catch (const std::exception&) {
        result.settings = Defaults();
        result.recovered_from_error = true;
        result.warning = "Settings were corrupt; safe defaults were used.";
        return result;
    }
}

bool SettingsStore::Save(const UserSettings& settings,
                         std::string& error) const {
    std::error_code filesystem_error;
    std::filesystem::create_directories(path_.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "The settings directory could not be created.";
        return false;
    }

    const nlohmann::json json{
        {"version", UserSettings::kCurrentVersion},
        {"default_output_folder", PathToUtf8(settings.default_output_folder)},
        {"preferred_image_format", ToString(settings.preferred_image_format)},
        {"maximum_concurrent_conversions",
         std::clamp<std::size_t>(settings.maximum_concurrent_conversions, 1,
                                 32)},
        {"hardware_acceleration", ToString(settings.hardware_acceleration)},
        {"preserve_metadata", settings.preserve_metadata},
        {"existing_file_policy", ToString(settings.existing_file_policy)},
        {"theme", ToString(settings.theme)},
    };

    const auto temporary = MakeTemporaryOutputPath(path_);
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            error = "The temporary settings file could not be opened.";
            return false;
        }
        output << json.dump(2) << '\n';
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary, filesystem_error);
            error = "The temporary settings file could not be written.";
            return false;
        }
    }

    if (!CommitTemporaryOutput(temporary, path_, true, error)) {
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    return true;
}

const std::filesystem::path& SettingsStore::Path() const noexcept {
    return path_;
}

std::filesystem::path SettingsStore::DefaultPath() {
#ifdef _WIN32
    wchar_t* local_app_data = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&local_app_data, &length, L"LOCALAPPDATA") == 0 &&
        local_app_data != nullptr) {
        const auto result = std::filesystem::path(local_app_data) /
                            "NativeShift" / "settings.json";
        std::free(local_app_data);
        return result;
    }
#endif
    return std::filesystem::temp_directory_path() / "NativeShift" /
           "settings.json";
}

std::filesystem::path SettingsStore::LegacyPath() {
#ifdef _WIN32
    wchar_t* local_app_data = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&local_app_data, &length, L"LOCALAPPDATA") == 0 &&
        local_app_data != nullptr) {
        const auto result = std::filesystem::path(local_app_data) /
                            "UniversalFileConverter" / "settings.json";
        std::free(local_app_data);
        return result;
    }
#endif
    return std::filesystem::temp_directory_path() / "UniversalFileConverter" /
           "settings.json";
}

} // namespace nativeshift::core
