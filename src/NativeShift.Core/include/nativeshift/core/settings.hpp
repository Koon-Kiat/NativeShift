#pragma once

#include "nativeshift/core/logger.hpp"
#include "nativeshift/core/models.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace nativeshift::core {

enum class HardwareAccelerationPreference { Auto, PreferHardware, Disabled };

enum class ThemePreference { System, Light, Dark };

struct UserSettings {
    static constexpr int kCurrentVersion = 2;

    int version{kCurrentVersion};
    std::filesystem::path default_output_folder;
    std::filesystem::path last_input_directory;
    std::filesystem::path last_output_directory;
    FileFormat preferred_image_format{FileFormat::Png};
    FileFormat preferred_audio_format{FileFormat::Mp3};
    FileFormat preferred_video_format{FileFormat::Mp4};
    std::size_t maximum_concurrent_conversions{0};
    HardwareAccelerationPreference hardware_acceleration{
        HardwareAccelerationPreference::Auto};
    bool preserve_metadata{false};
    OutputConflictPolicy existing_file_policy{
        OutputConflictPolicy::GenerateUniqueName};
    ThemePreference theme{ThemePreference::System};
    LogLevel logging_level{LogLevel::Information};
    bool notifications_enabled{true};
    std::vector<std::string> recent_presets;
};

struct SettingsLoadResult {
    UserSettings settings;
    bool recovered_from_error{false};
    bool migrated{false};
    std::string warning;
};

class SettingsStore {
  public:
    explicit SettingsStore(std::filesystem::path path = DefaultPath(),
                           std::filesystem::path legacy_path = {});

    [[nodiscard]] SettingsLoadResult Load() const;
    [[nodiscard]] bool Save(const UserSettings& settings,
                            std::string& error) const;
    [[nodiscard]] bool Reset(std::string& error) const;
    [[nodiscard]] const std::filesystem::path& Path() const noexcept;

    [[nodiscard]] static UserSettings DefaultSettings();
    [[nodiscard]] static std::filesystem::path DefaultPath();
    [[nodiscard]] static std::filesystem::path LegacyPath();

  private:
    std::filesystem::path path_;
    std::filesystem::path legacy_path_;
};

} // namespace nativeshift::core
