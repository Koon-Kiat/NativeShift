#include "nativeshift/core/output_paths.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#endif

namespace nativeshift::core {
namespace {

std::atomic<std::uint64_t> temporary_counter{1};

std::filesystem::path SafeFilenameStem(const std::filesystem::path& input) {
    auto stem = input.filename().stem();
    if (stem.empty() || stem == "." || stem == "..") {
        stem = "converted";
    }
    return stem;
}

} // namespace

std::filesystem::path
BuildOutputPath(const std::filesystem::path& input,
                const std::filesystem::path& output_directory,
                const FileFormat output_format) {
    const auto filename =
        SafeFilenameStem(input).native() +
        std::filesystem::path(ExtensionFor(output_format)).native();
    return (output_directory / filename).lexically_normal();
}

std::filesystem::path
GenerateUniqueOutputPath(const std::filesystem::path& desired) {
    std::error_code error;
    if (!std::filesystem::exists(desired, error) && !error) {
        return desired;
    }

    const auto parent = desired.parent_path();
    const auto stem = desired.stem().native();
    const auto extension = desired.extension().native();
    for (std::uint32_t index = 1; index < 100'000; ++index) {
        const auto suffix =
            std::filesystem::path(" (" + std::to_string(index) + ")").native();
        const auto candidate = parent / (stem + suffix + extension);
        error.clear();
        if (!std::filesystem::exists(candidate, error) && !error) {
            return candidate;
        }
    }
    return {};
}

OutputResolution ResolveOutputConflict(const std::filesystem::path& desired,
                                       const OutputConflictPolicy policy) {
    std::error_code error;
    const bool exists = std::filesystem::exists(desired, error);
    if (error) {
        return {{}, false, "The output path could not be inspected."};
    }
    if (!exists) {
        return {desired, false, {}};
    }

    switch (policy) {
    case OutputConflictPolicy::Skip:
        return {desired, true, {}};
    case OutputConflictPolicy::Replace:
        return {desired, false, {}};
    case OutputConflictPolicy::GenerateUniqueName: {
        auto unique = GenerateUniqueOutputPath(desired);
        if (unique.empty()) {
            return {
                {}, false, "A unique output filename could not be generated."};
        }
        return {std::move(unique), false, {}};
    }
    case OutputConflictPolicy::Ask:
    default:
        return {
            {},
            false,
            "The output exists and the conflict policy requires confirmation."};
    }
}

std::filesystem::path
MakeTemporaryOutputPath(const std::filesystem::path& final_path) {
    std::ostringstream suffix;
    suffix << ".nativeshift-";
#ifdef _WIN32
    std::array<unsigned char, 16> random_bytes{};
    const auto random_status = ::BCryptGenRandom(
        nullptr, random_bytes.data(), static_cast<ULONG>(random_bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (BCRYPT_SUCCESS(random_status)) {
        suffix << std::hex << std::setfill('0');
        for (const auto byte : random_bytes) {
            suffix << std::setw(2) << static_cast<unsigned int>(byte);
        }
    } else {
#endif
        const auto now = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto sequence = temporary_counter.fetch_add(1);
        suffix << std::hex << now << '-' << sequence;
#ifdef _WIN32
    }
#endif
    suffix << ".tmp";
    return final_path.parent_path() /
           (final_path.filename().native() +
            std::filesystem::path(suffix.str()).native());
}

bool CommitTemporaryOutput(const std::filesystem::path& temporary_path,
                           const std::filesystem::path& final_path,
                           const bool replace, std::string& error) {
#ifdef _WIN32
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (replace) {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    if (::MoveFileExW(temporary_path.c_str(), final_path.c_str(), flags) ==
        FALSE) {
        const auto code = static_cast<int>(::GetLastError());
        error = "The temporary output could not be committed (Windows error " +
                std::to_string(code) + ").";
        return false;
    }
    return true;
#else
    std::error_code filesystem_error;
    if (replace) {
        std::filesystem::remove(final_path, filesystem_error);
        filesystem_error.clear();
    }
    std::filesystem::rename(temporary_path, final_path, filesystem_error);
    if (filesystem_error) {
        error = "The temporary output could not be committed: " +
                filesystem_error.message();
        return false;
    }
    return true;
#endif
}

} // namespace nativeshift::core
