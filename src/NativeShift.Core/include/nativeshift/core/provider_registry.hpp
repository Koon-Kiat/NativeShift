#pragma once

#include "nativeshift/core/provider.hpp"

#include <memory>
#include <vector>

namespace nativeshift::core {

class ProviderRegistry {
  public:
    void Register(std::shared_ptr<IConversionProvider> provider);

    [[nodiscard]] std::shared_ptr<IConversionProvider>
    Select(FileFormat input, FileFormat output) const;
    [[nodiscard]] std::vector<ConversionCapabilities> Capabilities() const;
    [[nodiscard]] const std::vector<std::shared_ptr<IConversionProvider>>&
    Providers() const noexcept;

  private:
    std::vector<std::shared_ptr<IConversionProvider>> providers_;
};

} // namespace nativeshift::core
