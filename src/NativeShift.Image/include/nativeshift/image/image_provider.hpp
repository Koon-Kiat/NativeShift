#pragma once

#include "nativeshift/core/provider.hpp"

namespace nativeshift::image {

class ImageConversionProvider final : public core::IConversionProvider {
  public:
    [[nodiscard]] std::string Name() const override;
    [[nodiscard]] bool
    CanHandle(core::FileFormat input,
              core::FileFormat output) const noexcept override;
    [[nodiscard]] std::vector<core::ValidationIssue>
    Validate(const core::ConversionRequest& request) const override;
    [[nodiscard]] std::uintmax_t
    EstimateOutput(const core::ConversionRequest& request) const override;
    [[nodiscard]] core::ProviderOutcome
    Convert(const core::ConversionRequest& request,
            const core::ProgressCallback& progress,
            std::stop_token cancellation) override;
    [[nodiscard]] std::vector<core::FormatPair>
    GetSupportedFormats() const override;
    [[nodiscard]] std::vector<std::string> GetAvailableOptions() const override;
};

} // namespace nativeshift::image
