#pragma once

#include "nativeshift/core/logger.hpp"
#include "nativeshift/core/platform_services.hpp"
#include "nativeshift/core/provider_registry.hpp"

#include <memory>
#include <stop_token>
#include <vector>

namespace nativeshift::core {

class ConversionEngine {
  public:
    explicit ConversionEngine(Logger* logger = nullptr,
                              IPlatformServices* platform = nullptr);

    void RegisterProvider(std::shared_ptr<IConversionProvider> provider);
    [[nodiscard]] std::shared_ptr<IConversionProvider>
    SelectProvider(FileFormat input, FileFormat output) const;
    [[nodiscard]] std::vector<ConversionCapabilities> Capabilities() const;

    [[nodiscard]] ConversionResult Convert(ConversionRequest request,
                                           ProgressCallback progress = {},
                                           std::stop_token cancellation = {});

  private:
    ProviderRegistry providers_;
    Logger* logger_{};
    IPlatformServices* platform_{};
};

} // namespace nativeshift::core
