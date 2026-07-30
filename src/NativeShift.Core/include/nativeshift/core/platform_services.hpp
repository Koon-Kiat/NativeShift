#pragma once

#include <cstdint>
#include <filesystem>
#include <system_error>

namespace nativeshift::core {

struct StorageSpace {
    std::uintmax_t capacity{};
    std::uintmax_t available{};
};

class IPlatformServices {
  public:
    virtual ~IPlatformServices() = default;

    [[nodiscard]] virtual StorageSpace
    QueryStorageSpace(const std::filesystem::path& path,
                      std::error_code& error) const = 0;
    [[nodiscard]] virtual std::filesystem::path
    ApplicationDataDirectory() const = 0;
    [[nodiscard]] virtual std::filesystem::path TemporaryDirectory() const = 0;
};

} // namespace nativeshift::core
