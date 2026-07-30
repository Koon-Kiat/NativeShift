#include "nativeshift/core/provider_registry.hpp"

#include <utility>

namespace nativeshift::core {

void ProviderRegistry::Register(std::shared_ptr<IConversionProvider> provider) {
    if (provider) {
        providers_.push_back(std::move(provider));
    }
}

std::shared_ptr<IConversionProvider>
ProviderRegistry::Select(const FileFormat input,
                         const FileFormat output) const {
    for (const auto& provider : providers_) {
        if (provider->CanHandle(input, output)) {
            return provider;
        }
    }
    return {};
}

std::vector<ConversionCapabilities> ProviderRegistry::Capabilities() const {
    std::vector<ConversionCapabilities> capabilities;
    capabilities.reserve(providers_.size());
    for (const auto& provider : providers_) {
        capabilities.push_back({provider->Name(),
                                provider->GetSupportedFormats(),
                                provider->GetAvailableOptions()});
    }
    return capabilities;
}

const std::vector<std::shared_ptr<IConversionProvider>>&
ProviderRegistry::Providers() const noexcept {
    return providers_;
}

} // namespace nativeshift::core
