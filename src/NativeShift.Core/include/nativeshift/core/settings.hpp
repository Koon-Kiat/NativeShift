#pragma once

#include "nativeshift/core/models.hpp"

#include <filesystem>
#include <string>

namespace nativeshift::core {

enum class HardwareAccelerationPreference { Auto, PreferHardware, Disabled };

enum class ThemePreference { System, Light, Dark };

struct UserSettings {
    static constexpr int kCurrentVersion = 1;

    int version{kCurrentVersion};
    std::filesystem::path default_output_folder;
    FileFormat preferred_image_format{FileFormat::Png};
    std::size_t maximum_concurrent_conversions{0};
    HardwareAccelerationPreference hardware_acceleration{
        HardwareAccelerationPreference::Auto};
    bool preserve_metadata{false};
    OutputConflictPolicy existing_file_policy{
        OutputConflictPolicy::GenerateUniqueName};
    ThemePreference theme{ThemePreference::System};
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
    [[nodiscard]] const std::filesystem::path& Path() const noexcept;

    [[nodiscard]] static std::filesystem::path DefaultPath();
    [[nodiscard]] static std::filesystem::path LegacyPath();

  private:
    std::filesystem::path path_;
    std::filesystem::path legacy_path_;
};

} // namespace nativeshift::core
