#include "nativeshift/core/conversion_engine.hpp"
#include "nativeshift/core/format_detector.hpp"
#include "nativeshift/core/job_queue.hpp"
#include "nativeshift/core/output_paths.hpp"
#include "nativeshift/core/settings.hpp"
#include "nativeshift/core/validation.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

using namespace std::chrono_literals;
using nativeshift::core::ConversionEngine;
using nativeshift::core::ConversionRequest;
using nativeshift::core::ConversionStatus;
using nativeshift::core::ErrorCategory;
using nativeshift::core::FileFormat;
using nativeshift::core::FormatPair;
using nativeshift::core::IConversionProvider;
using nativeshift::core::OutputConflictPolicy;
using nativeshift::core::ProgressCallback;
using nativeshift::core::ProviderOutcome;
using nativeshift::core::ValidationIssue;

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{1};
        path_ = std::filesystem::temp_directory_path() /
                ("nativeshift-core-tests-" +
#ifdef _WIN32
                 std::to_string(::GetCurrentProcessId()) + "-" +
#endif
                 std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(Extended(path_), ignored);
    }

    [[nodiscard]] const std::filesystem::path& Path() const noexcept {
        return path_;
    }

  private:
    static std::filesystem::path Extended(const std::filesystem::path& path) {
#ifdef _WIN32
        const auto absolute = std::filesystem::absolute(path).native();
        if (absolute.starts_with(LR"(\\?\)")) {
            return std::filesystem::path(absolute);
        }
        return std::filesystem::path(std::wstring(LR"(\\?\)") + absolute);
#else
        return path;
#endif
    }

    std::filesystem::path path_;
};

void WriteBytes(const std::filesystem::path& path,
                const std::initializer_list<std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary);
    for (const auto byte : bytes) {
        output.put(static_cast<char>(byte));
    }
}

void WritePngSignature(const std::filesystem::path& path) {
    WriteBytes(path, {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A});
}

class FakeProvider final : public IConversionProvider {
  public:
    std::chrono::milliseconds delay{0};
    bool fail{false};
    bool write_temporary_output{true};
    std::atomic_int active{0};
    std::atomic_int maximum_active{0};

    [[nodiscard]] std::string Name() const override { return "FakeProvider"; }

    [[nodiscard]] bool
    CanHandle(const FileFormat input,
              const FileFormat output) const noexcept override {
        return input == FileFormat::Png && output == FileFormat::Jpeg;
    }

    [[nodiscard]] std::vector<ValidationIssue>
    Validate(const ConversionRequest&) const override {
        return {};
    }

    [[nodiscard]] std::uintmax_t
    EstimateOutput(const ConversionRequest&) const override {
        return 16;
    }

    [[nodiscard]] ProviderOutcome
    Convert(const ConversionRequest& request, const ProgressCallback&,
            const std::stop_token cancellation) override {
        const int current = active.fetch_add(1) + 1;
        int observed = maximum_active.load();
        while (current > observed &&
               !maximum_active.compare_exchange_weak(observed, current)) {
        }

        const auto deadline = std::chrono::steady_clock::now() + delay;
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancellation.stop_requested()) {
                active.fetch_sub(1);
                return ProviderOutcome::Cancelled("Cancelled by test.");
            }
            std::this_thread::sleep_for(2ms);
        }
        if (write_temporary_output) {
            std::ofstream output(request.output_path, std::ios::binary);
            output << "converted";
        }
        active.fetch_sub(1);
        if (fail) {
            return ProviderOutcome::Failed(ErrorCategory::Codec,
                                           "Synthetic provider failure.");
        }
        return ProviderOutcome::Succeeded();
    }

    [[nodiscard]] std::vector<FormatPair> GetSupportedFormats() const override {
        return {{FileFormat::Png, FileFormat::Jpeg}};
    }

    [[nodiscard]] std::vector<std::string>
    GetAvailableOptions() const override {
        return {};
    }
};

ConversionRequest MakeRequest(const std::filesystem::path& input,
                              const std::filesystem::path& output) {
    ConversionRequest request;
    request.input_path = input;
    request.output_path = output;
    request.output_format = FileFormat::Jpeg;
    request.conflict_policy = OutputConflictPolicy::Replace;
    return request;
}

TEST(FormatDetection, DetectsContentInsteadOfExtension) {
    TemporaryDirectory directory;
    const auto png = directory.Path() / "renamed.txt";
    const auto jpeg = directory.Path() / "photo.bin";
    const auto webp = directory.Path() / "image.data";
    WritePngSignature(png);
    WriteBytes(jpeg, {0xFF, 0xD8, 0xFF, 0xE0});
    WriteBytes(webp, {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'});

    EXPECT_EQ(nativeshift::core::DetectFormat(png).format, FileFormat::Png);
    EXPECT_EQ(nativeshift::core::DetectFormat(jpeg).format, FileFormat::Jpeg);
    EXPECT_EQ(nativeshift::core::DetectFormat(webp).format, FileFormat::WebP);
}

TEST(FormatDetection, RejectsUnknownContent) {
    TemporaryDirectory directory;
    const auto corrupt = directory.Path() / "fake.png";
    WriteBytes(corrupt, {'n', 'o', 't', 'p', 'n', 'g'});
    EXPECT_EQ(nativeshift::core::DetectFormat(corrupt).format,
              FileFormat::Unknown);
}

TEST(Validation, RejectsSameInputAndOutputAndBadQuality) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "source.png";
    WritePngSignature(input);
    auto request = MakeRequest(input, input);
    request.input_format = FileFormat::Png;
    request.image.quality = 101;

    const auto issues = nativeshift::core::ValidateRequestBasics(request);
    EXPECT_TRUE(std::ranges::any_of(issues, [](const auto& issue) {
        return issue.code == "same_input_output";
    }));
    EXPECT_TRUE(std::ranges::any_of(issues, [](const auto& issue) {
        return issue.code == "quality_range";
    }));
}

TEST(OutputPaths, UsesOnlyFilenameAndKeepsOutputBoundary) {
    const std::filesystem::path output = L"C:\\safe\\output";
    const auto result = nativeshift::core::BuildOutputPath(
        L"..\\..\\outside\\写真.png", output, FileFormat::WebP);
    EXPECT_EQ(result.parent_path(), output);
    EXPECT_EQ(result.filename(), L"写真.webp");
}

TEST(OutputPaths, AppliesConflictPolicies) {
    TemporaryDirectory directory;
    const auto desired = directory.Path() / "result.jpg";
    std::ofstream(desired) << "existing";

    EXPECT_TRUE(nativeshift::core::ResolveOutputConflict(
                    desired, OutputConflictPolicy::Skip)
                    .skip);
    EXPECT_FALSE(nativeshift::core::ResolveOutputConflict(
                     desired, OutputConflictPolicy::Ask)
                     .error.empty());
    const auto unique = nativeshift::core::ResolveOutputConflict(
        desired, OutputConflictPolicy::GenerateUniqueName);
    EXPECT_FALSE(unique.path.empty());
    EXPECT_NE(unique.path, desired);
    EXPECT_EQ(unique.path.parent_path(), desired.parent_path());
}

TEST(Settings, RecoversFromCorruptSettings) {
    TemporaryDirectory directory;
    const auto path = directory.Path() / "settings.json";
    std::ofstream(path) << "{ definitely not json";
    nativeshift::core::SettingsStore store(path);

    const auto loaded = store.Load();
    EXPECT_TRUE(loaded.recovered_from_error);
    EXPECT_GE(loaded.settings.maximum_concurrent_conversions, 1U);
}

TEST(Settings, MigratesLegacyVersionAndRoundTripsUnicode) {
    TemporaryDirectory directory;
    const auto path = directory.Path() / "settings.json";
    std::ofstream(path)
        << R"({"version":0,"max_concurrent_jobs":3,"keep_metadata":true,)"
        << R"("output_folder":"C:\\変換"})";
    nativeshift::core::SettingsStore store(path);

    const auto loaded = store.Load();
    ASSERT_TRUE(loaded.migrated);
    EXPECT_EQ(loaded.settings.maximum_concurrent_conversions, 3U);
    EXPECT_TRUE(loaded.settings.preserve_metadata);

    std::string error;
    EXPECT_TRUE(store.Save(loaded.settings, error)) << error;
    const auto round_trip = store.Load();
    EXPECT_FALSE(round_trip.recovered_from_error);
    EXPECT_EQ(round_trip.settings.default_output_folder,
              loaded.settings.default_output_folder);
}

TEST(Settings, MigratesLegacyApplicationIdentityOnce) {
    TemporaryDirectory directory;
    const auto current_path =
        directory.Path() / "NativeShift" / "settings.json";
    const auto legacy_path =
        directory.Path() / "UniversalFileConverter" / "settings.json";
    std::filesystem::create_directories(legacy_path.parent_path());
    std::ofstream(legacy_path)
        << R"({"version":1,"maximum_concurrent_conversions":2,)"
        << R"("preferred_image_format":"webp"})";

    nativeshift::core::SettingsStore store(current_path, legacy_path);
    const auto loaded = store.Load();

    ASSERT_TRUE(loaded.migrated);
    EXPECT_EQ(loaded.settings.maximum_concurrent_conversions, 2U);
    EXPECT_EQ(loaded.settings.preferred_image_format, FileFormat::WebP);
    EXPECT_TRUE(std::filesystem::exists(current_path));
    EXPECT_FALSE(std::filesystem::exists(legacy_path));
    auto backup_path = legacy_path;
    backup_path += L".migration.bak";
    EXPECT_TRUE(std::filesystem::exists(backup_path));

    const auto second_load = store.Load();
    EXPECT_FALSE(second_load.migrated);
}

TEST(ConversionEngine, SelectsProviderAndCommitsTemporaryOutput) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "source.png";
    const auto output = directory.Path() / "result.jpg";
    WritePngSignature(input);
    auto provider = std::make_shared<FakeProvider>();
    ConversionEngine engine;
    engine.RegisterProvider(provider);

    EXPECT_EQ(engine.SelectProvider(FileFormat::Png, FileFormat::Jpeg),
              provider);
    const auto result = engine.Convert(MakeRequest(input, output));
    EXPECT_EQ(result.status, ConversionStatus::Success);
    EXPECT_TRUE(std::filesystem::exists(output));
}

TEST(ConversionEngine, CleansTemporaryOutputAfterProviderFailure) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "source.png";
    const auto output = directory.Path() / "result.jpg";
    WritePngSignature(input);
    auto provider = std::make_shared<FakeProvider>();
    provider->fail = true;
    ConversionEngine engine;
    engine.RegisterProvider(provider);

    const auto result = engine.Convert(MakeRequest(input, output));
    EXPECT_EQ(result.status, ConversionStatus::Failed);
    EXPECT_FALSE(std::filesystem::exists(output));
    for (const auto& entry :
         std::filesystem::directory_iterator(directory.Path())) {
        EXPECT_EQ(entry.path().filename().native().find(L".nativeshift-"),
                  std::wstring::npos);
    }
}

TEST(ConversionEngine, RejectsUnsupportedAndCorruptInput) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "fake.png";
    const auto output = directory.Path() / "result.jpg";
    WriteBytes(input, {'b', 'a', 'd'});
    ConversionEngine engine;
    engine.RegisterProvider(std::make_shared<FakeProvider>());

    const auto result = engine.Convert(MakeRequest(input, output));
    EXPECT_EQ(result.status, ConversionStatus::Failed);
    EXPECT_EQ(result.error, ErrorCategory::UnsupportedFormat);
}

TEST(JobQueue, EnforcesMaximumConcurrencyAndIsolatesFailures) {
    TemporaryDirectory directory;
    auto provider = std::make_shared<FakeProvider>();
    provider->delay = 30ms;
    ConversionEngine engine;
    engine.RegisterProvider(provider);
    nativeshift::core::JobQueue queue(engine, 2, 8);
    std::vector<nativeshift::core::JobHandle> handles;

    for (int index = 0; index < 6; ++index) {
        const auto input =
            directory.Path() / ("input-" + std::to_string(index) + ".png");
        const auto output =
            directory.Path() / ("output-" + std::to_string(index) + ".jpg");
        WritePngSignature(input);
        auto handle = queue.TrySubmit(MakeRequest(input, output));
        ASSERT_TRUE(handle.has_value());
        handles.push_back(std::move(*handle));
    }
    for (const auto& handle : handles) {
        EXPECT_EQ(handle.Get().status, ConversionStatus::Success);
    }
    EXPECT_GE(provider->maximum_active.load(), 2);
    EXPECT_LE(provider->maximum_active.load(), 2);
}

TEST(JobQueue, CancelsAJobCooperatively) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "source.png";
    const auto output = directory.Path() / "result.jpg";
    WritePngSignature(input);
    auto provider = std::make_shared<FakeProvider>();
    provider->delay = 1s;
    ConversionEngine engine;
    engine.RegisterProvider(provider);
    nativeshift::core::JobQueue queue(engine, 1, 2);

    auto handle = queue.TrySubmit(MakeRequest(input, output));
    ASSERT_TRUE(handle.has_value());
    handle->Cancel();
    EXPECT_EQ(handle->Get().status, ConversionStatus::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(JobQueue, PausesPendingWorkReliably) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "source.png";
    const auto output = directory.Path() / "result.jpg";
    WritePngSignature(input);
    auto provider = std::make_shared<FakeProvider>();
    ConversionEngine engine;
    engine.RegisterProvider(provider);
    nativeshift::core::JobQueue queue(engine, 1, 2);
    queue.Pause();

    auto handle = queue.TrySubmit(MakeRequest(input, output));
    ASSERT_TRUE(handle.has_value());
    EXPECT_EQ(handle->WaitFor(30ms), std::future_status::timeout);
    queue.Resume();
    EXPECT_EQ(handle->Get().status, ConversionStatus::Success);
}

TEST(JobQueue, CancelsPendingWorkWhilePaused) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "source.png";
    const auto output = directory.Path() / "result.jpg";
    WritePngSignature(input);
    ConversionEngine engine;
    engine.RegisterProvider(std::make_shared<FakeProvider>());
    nativeshift::core::JobQueue queue(engine, 1, 2);
    queue.Pause();

    auto handle = queue.TrySubmit(MakeRequest(input, output));
    ASSERT_TRUE(handle.has_value());
    handle->Cancel();
    ASSERT_EQ(handle->WaitFor(1s), std::future_status::ready);
    EXPECT_EQ(handle->Get().status, ConversionStatus::Cancelled);
}

TEST(ConversionEngine, HandlesExtendedWindowsLongPaths) {
#ifdef _WIN32
    TemporaryDirectory directory;
    auto long_directory = directory.Path();
    for (int index = 0; index < 8; ++index) {
        long_directory /= L"long-segment-abcdefghijklmnopqrstuvwxyz-0123456789";
    }
    const auto absolute = std::filesystem::absolute(long_directory).native();
    const auto extended_directory =
        std::filesystem::path(std::wstring(LR"(\\?\)") + absolute);
    std::filesystem::create_directories(extended_directory);
    const auto input = extended_directory / L"入力.png";
    const auto output = extended_directory / L"出力.jpg";
    ASSERT_GT(input.native().size(), 260U);
    WritePngSignature(input);
    ConversionEngine engine;
    engine.RegisterProvider(std::make_shared<FakeProvider>());

    const auto result = engine.Convert(MakeRequest(input, output));
    EXPECT_EQ(result.status, ConversionStatus::Success) << result.message;
    EXPECT_TRUE(std::filesystem::exists(output));
#else
    GTEST_SKIP() << "Windows extended paths are Windows-specific.";
#endif
}

} // namespace
