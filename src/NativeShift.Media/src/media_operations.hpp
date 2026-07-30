#pragma once

#include "nativeshift/core/models.hpp"

#include <stop_token>

namespace nativeshift::media {

[[nodiscard]] core::ProviderOutcome
TranscodeAudio(const core::ConversionRequest& request,
               const core::ProgressCallback& progress,
               std::stop_token cancellation);

[[nodiscard]] core::ProviderOutcome
TranscodeVideo(const core::ConversionRequest& request,
               const core::ProgressCallback& progress,
               std::stop_token cancellation);

[[nodiscard]] core::ProviderOutcome
TranscodeVideoEncoded(const core::ConversionRequest& request,
                      const core::ProgressCallback& progress,
                      std::stop_token cancellation);

} // namespace nativeshift::media
