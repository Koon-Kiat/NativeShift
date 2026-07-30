#include "nativeshift/core/validation.hpp"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <string>

namespace nativeshift::core {
namespace {

std::filesystem::path
NormalizedForComparison(const std::filesystem::path& value) {
    std::error_code error;
    auto normalized =
        std::filesystem::absolute(value, error).lexically_normal();
    if (error) {
        normalized = value.lexically_normal();
    }
#ifdef _WIN32
    auto native = normalized.native();
    std::ranges::transform(native, native.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return std::filesystem::path(native);
#else
    return normalized;
#endif
}

void AddIssue(std::vector<ValidationIssue>& issues,
              const ErrorCategory category, std::string code,
              std::string message) {
    issues.push_back(
        ValidationIssue{category, std::move(code), std::move(message)});
}

} // namespace

std::vector<ValidationIssue>
ValidateRequestBasics(const ConversionRequest& request) {
    std::vector<ValidationIssue> issues;

    if (request.input_path.empty()) {
        AddIssue(issues, ErrorCategory::InvalidRequest, "input_required",
                 "An input path is required.");
    } else {
        std::error_code error;
        if (!std::filesystem::exists(request.input_path, error) || error) {
            AddIssue(issues, ErrorCategory::InvalidInput, "input_missing",
                     "The input file does not exist or is inaccessible.");
        } else if (!std::filesystem::is_regular_file(request.input_path,
                                                     error) ||
                   error) {
            AddIssue(issues, ErrorCategory::InvalidInput, "input_not_file",
                     "The input path is not a regular file.");
        } else {
            const auto size =
                std::filesystem::file_size(request.input_path, error);
            if (!error && size > kMaximumEncodedImageBytes &&
                KindOf(request.input_format) == MediaKind::Image) {
                AddIssue(issues, ErrorCategory::ResourceLimit,
                         "input_too_large",
                         "The encoded image exceeds the Phase 1 safety limit.");
            }
        }
    }

    if (request.output_path.empty()) {
        AddIssue(issues, ErrorCategory::InvalidRequest, "output_required",
                 "An output path is required.");
    } else {
        auto parent = request.output_path.parent_path();
        if (parent.empty()) {
            parent = std::filesystem::current_path();
        }
        std::error_code error;
        if (!std::filesystem::exists(parent, error) || error ||
            !std::filesystem::is_directory(parent, error)) {
            AddIssue(issues, ErrorCategory::InvalidRequest,
                     "output_directory_missing",
                     "The output directory does not exist or is inaccessible.");
        }
    }

    if (!request.input_path.empty() && !request.output_path.empty() &&
        NormalizedForComparison(request.input_path) ==
            NormalizedForComparison(request.output_path)) {
        AddIssue(issues, ErrorCategory::InvalidRequest, "same_input_output",
                 "The input and output paths must be different.");
    }

    if (request.input_format == FileFormat::Unknown) {
        AddIssue(issues, ErrorCategory::UnsupportedFormat,
                 "unknown_input_format",
                 "The input format could not be detected from file content.");
    }
    if (request.output_format == FileFormat::Unknown) {
        AddIssue(issues, ErrorCategory::InvalidRequest,
                 "output_format_required", "An output format is required.");
    }

    if (request.image.quality < 1 || request.image.quality > 100) {
        AddIssue(issues, ErrorCategory::InvalidRequest, "quality_range",
                 "Image quality must be between 1 and 100.");
    }

    const auto validate_dimension =
        [&issues](const std::optional<std::uint32_t> dimension,
                  const char* code, const char* label) {
            if (dimension.has_value() &&
                (*dimension == 0 || *dimension > kMaximumImageDimension)) {
                AddIssue(
                    issues, ErrorCategory::ResourceLimit, code,
                    std::string(label) +
                        " must be between 1 and the configured safety limit.");
            }
        };
    validate_dimension(request.image.width, "width_range", "Image width");
    validate_dimension(request.image.height, "height_range", "Image height");

    if (request.image.width.has_value() && request.image.height.has_value()) {
        const auto pixels = static_cast<std::uint64_t>(*request.image.width) *
                            static_cast<std::uint64_t>(*request.image.height);
        if (pixels > kMaximumImagePixels) {
            AddIssue(issues, ErrorCategory::ResourceLimit, "pixel_limit",
                     "The requested output dimensions exceed the pixel safety "
                     "limit.");
        }
    }

    return issues;
}

} // namespace nativeshift::core
