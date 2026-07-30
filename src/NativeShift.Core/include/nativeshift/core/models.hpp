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

struct ConversionRequest {
    std::filesystem::path input_path;
    std::filesystem::path output_path;
    FileFormat input_format{FileFormat::Unknown};
    FileFormat output_format{FileFormat::Unknown};
    OutputConflictPolicy conflict_policy{OutputConflictPolicy::Ask};
    ImageOptions image;
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
    std::vector<std::string> warnings;

    [[nodiscard]] static ProviderOutcome Succeeded();
    [[nodiscard]] static ProviderOutcome Failed(ErrorCategory category,
                                                std::string message);
    [[nodiscard]] static ProviderOutcome Cancelled(std::string message);
};

struct ConversionResult {
    ConversionStatus status{ConversionStatus::Failed};
    ErrorCategory error{ErrorCategory::Internal};
    std::string message;
    std::string provider;
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
