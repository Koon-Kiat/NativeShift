#pragma once

#include "nativeshift/core/platform_services.hpp"

namespace nativeshift::platform {

class WindowsPlatformServices final : public core::IPlatformServices {
  public:
    [[nodiscard]] core::StorageSpace
    QueryStorageSpace(const std::filesystem::path& path,
                      std::error_code& error) const override;
    [[nodiscard]] std::filesystem::path
    ApplicationDataDirectory() const override;
    [[nodiscard]] std::filesystem::path TemporaryDirectory() const override;
};

} // namespace nativeshift::platform
