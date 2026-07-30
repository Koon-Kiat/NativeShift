#pragma once

#include "nativeshift/core/models.hpp"

#include <cstdint>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace nativeshift::core {

struct FormatPair {
    FileFormat input{FileFormat::Unknown};
    FileFormat output{FileFormat::Unknown};

    auto operator<=>(const FormatPair&) const = default;
};

struct ConversionCapabilities {
    std::string provider;
    std::vector<FormatPair> formats;
    std::vector<std::string> options;
};

class IConversionProvider {
  public:
    virtual ~IConversionProvider() = default;

    [[nodiscard]] virtual std::string Name() const = 0;
    [[nodiscard]] virtual bool CanHandle(FileFormat input,
                                         FileFormat output) const noexcept = 0;
    [[nodiscard]] virtual std::vector<ValidationIssue>
    Validate(const ConversionRequest& request) const = 0;
    [[nodiscard]] virtual std::uintmax_t
    EstimateOutput(const ConversionRequest& request) const = 0;

    // The bounded job queue invokes this on a worker thread. Keeping provider
    // work synchronous here prevents hidden provider threads from bypassing
    // the scheduler's concurrency and memory limits.
    [[nodiscard]] virtual ProviderOutcome
    Convert(const ConversionRequest& request, const ProgressCallback& progress,
            std::stop_token cancellation) = 0;

    [[nodiscard]] virtual std::vector<FormatPair>
    GetSupportedFormats() const = 0;
    [[nodiscard]] virtual std::vector<std::string>
    GetAvailableOptions() const = 0;
};

} // namespace nativeshift::core
