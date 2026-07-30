#include "nativeshift/platform/windows_platform_services.hpp"

#include <Windows.h>
#include <knownfolders.h>
#include <shlobj.h>

namespace nativeshift::platform {

core::StorageSpace
WindowsPlatformServices::QueryStorageSpace(const std::filesystem::path& path,
                                           std::error_code& error) const {
    const auto value = std::filesystem::space(path, error);
    return {value.capacity, value.available};
}

std::filesystem::path
WindowsPlatformServices::ApplicationDataDirectory() const {
    PWSTR raw_path = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
                                         nullptr, &raw_path))) {
        const std::filesystem::path path(raw_path);
        ::CoTaskMemFree(raw_path);
        return path / L"NativeShift";
    }
    return std::filesystem::temp_directory_path() / L"NativeShift";
}

std::filesystem::path WindowsPlatformServices::TemporaryDirectory() const {
    return std::filesystem::temp_directory_path() / L"NativeShift";
}

} // namespace nativeshift::platform
