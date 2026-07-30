#pragma once

#include "nativeshift/core/models.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nativeshift::core {

struct ConversionPreset {
    std::string id;
    std::string name;
    MediaKind media_kind{MediaKind::Unknown};
    FileFormat output_format{FileFormat::Unknown};
    ImageOptions image;
    AudioOptions audio;
    VideoOptions video;
    bool built_in{false};
};

struct PresetLoadResult {
    std::vector<ConversionPreset> presets;
    bool recovered_from_error{false};
    bool migrated{false};
    std::string warning;
};

class PresetStore {
  public:
    static constexpr int kCurrentVersion = 1;

    explicit PresetStore(std::filesystem::path path = DefaultPath());

    [[nodiscard]] PresetLoadResult LoadCustom() const;
    [[nodiscard]] bool SaveCustom(const std::vector<ConversionPreset>& presets,
                                  std::string& error) const;
    [[nodiscard]] const std::filesystem::path& Path() const noexcept;

    [[nodiscard]] static std::vector<ConversionPreset> BuiltIns();
    [[nodiscard]] static std::optional<ConversionPreset>
    Duplicate(const ConversionPreset& source, std::string name);
    [[nodiscard]] static bool Rename(ConversionPreset& preset,
                                     std::string name);
    [[nodiscard]] static bool Delete(std::vector<ConversionPreset>& presets,
                                     std::string_view id);
    [[nodiscard]] static std::vector<ValidationIssue>
    Validate(const ConversionPreset& preset, FileFormat input_format);
    [[nodiscard]] static std::filesystem::path DefaultPath();

  private:
    std::filesystem::path path_;
};

} // namespace nativeshift::core
