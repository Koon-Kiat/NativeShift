#include "nativeshift/gui_bridge.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

std::wstring Read(std::size_t (*reader)(wchar_t*, std::size_t)) {
    const auto required = reader(nullptr, 0);
    EXPECT_GT(required, 1U);
    std::vector<wchar_t> buffer(required);
    EXPECT_EQ(reader(buffer.data(), buffer.size()), required);
    return buffer.data();
}

TEST(GuiBridge, ReportsCapabilitiesAndPresetsAsJson) {
    const auto capabilities = Read(nativeshift_capabilities_json);
    const auto presets = Read(nativeshift_presets_json);
    EXPECT_NE(capabilities.find(L"ffmpeg_version"), std::wstring::npos);
    EXPECT_NE(presets.find(L"jpeg-high"), std::wstring::npos);
}

TEST(GuiBridge, DetectsFormatByContentForImmediateQueueFeedback) {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path =
        std::filesystem::temp_directory_path() /
        ("nativeshift-gui-detect-" + std::to_string(suffix) + ".txt");
    {
        std::ofstream output(path, std::ios::binary);
        constexpr unsigned char png_signature[]{0x89, 0x50, 0x4E, 0x47,
                                                0x0D, 0x0A, 0x1A, 0x0A};
        for (const auto byte : png_signature) {
            output.put(static_cast<char>(byte));
        }
    }

    const auto required = nativeshift_detect_format(path.c_str(), nullptr, 0);
    ASSERT_GT(required, 1U);
    std::vector<wchar_t> buffer(required);
    EXPECT_EQ(
        nativeshift_detect_format(path.c_str(), buffer.data(), buffer.size()),
        required);
    EXPECT_EQ(std::wstring(buffer.data()), L"png");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST(GuiBridge, ReportsSettingsLogsAndPrivacySafeDiagnostics) {
    const auto settings = Read(nativeshift_settings_json);
    const auto diagnostics = Read(nativeshift_diagnostic_summary);
    const auto log_folder = Read(nativeshift_log_folder);
    EXPECT_NE(settings.find(L"maximum_concurrent_conversions"),
              std::wstring::npos);
    EXPECT_NE(diagnostics.find(L"NativeShift"), std::wstring::npos);
    EXPECT_FALSE(log_folder.empty());
}

TEST(GuiBridge, RejectsInvalidSubmissionWithoutThrowingAcrossAbi) {
    EXPECT_EQ(nativeshift_submit(nullptr, nullptr, nullptr, nullptr), 0U);
    EXPECT_NE(Read(nativeshift_last_error).find(L"required"),
              std::wstring::npos);
}

TEST(GuiBridge, RejectsRetryForMissingJob) {
    EXPECT_EQ(nativeshift_retry(999999), 0U);
    EXPECT_NE(Read(nativeshift_last_error).find(L"no longer exists"),
              std::wstring::npos);
}

} // namespace
