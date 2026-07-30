#include "nativeshift/core/conversion_engine.hpp"
#include "nativeshift/core/format_detector.hpp"
#include "nativeshift/image/image_provider.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <png.h>
#include <webp/decode.h>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

using nativeshift::core::ConversionEngine;
using nativeshift::core::ConversionRequest;
using nativeshift::core::ConversionStatus;
using nativeshift::core::ErrorCategory;
using nativeshift::core::FileFormat;
using nativeshift::core::OutputConflictPolicy;

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{1};
        path_ = std::filesystem::temp_directory_path() /
                ("nativeshift-image-tests-" +
#ifdef _WIN32
                 std::to_string(::GetCurrentProcessId()) + "-" +
#endif
                 std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& Path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

struct FileCloser {
    void operator()(std::FILE* file) const noexcept {
        if (file != nullptr) {
            std::fclose(file);
        }
    }
};

bool CreateTinyPng(const std::filesystem::path& path) {
    std::FILE* raw = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&raw, path.c_str(), L"wb") != 0) {
        return false;
    }
#else
    raw = std::fopen(path.string().c_str(), "wb");
#endif
    std::unique_ptr<std::FILE, FileCloser> file(raw);
    if (!file) {
        return false;
    }
    constexpr std::array<std::uint8_t, 16> rgba{
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 128};
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = 2;
    image.height = 2;
    image.format = PNG_FORMAT_RGBA;
    return png_image_write_to_stdio(&image, file.get(), 0, rgba.data(), 0,
                                    nullptr) != 0;
}

ConversionEngine MakeEngine() {
    ConversionEngine engine;
    engine.RegisterProvider(
        std::make_shared<nativeshift::image::ImageConversionProvider>());
    return engine;
}

ConversionRequest Request(const std::filesystem::path& input,
                          const std::filesystem::path& output,
                          const FileFormat format) {
    ConversionRequest request;
    request.input_path = input;
    request.output_path = output;
    request.output_format = format;
    request.conflict_policy = OutputConflictPolicy::Replace;
    return request;
}

TEST(ImageIntegration, ConvertsPngToJpeg) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / L"入力.png";
    const auto output = directory.Path() / L"出力.jpg";
    ASSERT_TRUE(CreateTinyPng(input));
    auto engine = MakeEngine();

    const auto result =
        engine.Convert(Request(input, output, FileFormat::Jpeg));
    ASSERT_EQ(result.status, ConversionStatus::Success) << result.message;
    EXPECT_EQ(nativeshift::core::DetectFormat(output).format, FileFormat::Jpeg);
}

TEST(ImageIntegration, ConvertsJpegToWebPAndResizes) {
    TemporaryDirectory directory;
    const auto png = directory.Path() / "source.png";
    const auto jpeg = directory.Path() / "source.jpg";
    const auto webp = directory.Path() / "result.webp";
    ASSERT_TRUE(CreateTinyPng(png));
    auto engine = MakeEngine();
    ASSERT_EQ(engine.Convert(Request(png, jpeg, FileFormat::Jpeg)).status,
              ConversionStatus::Success);

    auto request = Request(jpeg, webp, FileFormat::WebP);
    request.image.width = 4;
    request.image.height = 4;
    request.image.preserve_aspect_ratio = true;
    const auto result = engine.Convert(request);
    ASSERT_EQ(result.status, ConversionStatus::Success) << result.message;
    EXPECT_EQ(nativeshift::core::DetectFormat(webp).format, FileFormat::WebP);

    std::ifstream input(webp, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(input);
    const auto size = input.tellg();
    ASSERT_GT(size, 0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    int width = 0;
    int height = 0;
    ASSERT_NE(WebPGetInfo(bytes.data(), bytes.size(), &width, &height), 0);
    EXPECT_EQ(width, 4);
    EXPECT_EQ(height, 4);
}

TEST(ImageIntegration, RemovesPartialOutputForInvalidInput) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "corrupt.png";
    const auto output = directory.Path() / "result.webp";
    {
        std::ofstream stream(input, std::ios::binary);
        constexpr std::array<std::uint8_t, 12> corrupt{0x89, 0x50, 0x4E, 0x47,
                                                       0x0D, 0x0A, 0x1A, 0x0A,
                                                       0x00, 0x00, 0x00, 0x00};
        stream.write(reinterpret_cast<const char*>(corrupt.data()),
                     static_cast<std::streamsize>(corrupt.size()));
    }
    auto engine = MakeEngine();

    const auto result =
        engine.Convert(Request(input, output, FileFormat::WebP));
    EXPECT_EQ(result.status, ConversionStatus::Failed);
    EXPECT_EQ(result.error, ErrorCategory::InvalidInput);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(ImageIntegration, HonorsPreCancelledToken) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "source.png";
    const auto output = directory.Path() / "result.webp";
    ASSERT_TRUE(CreateTinyPng(input));
    auto engine = MakeEngine();
    std::stop_source cancellation;
    cancellation.request_stop();

    const auto result = engine.Convert(Request(input, output, FileFormat::WebP),
                                       {}, cancellation.get_token());
    EXPECT_EQ(result.status, ConversionStatus::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(ImageIntegration, WarnsWhenMetadataPreservationIsRequested) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "source.png";
    const auto output = directory.Path() / "result.webp";
    ASSERT_TRUE(CreateTinyPng(input));
    auto engine = MakeEngine();
    auto request = Request(input, output, FileFormat::WebP);
    request.image.preserve_metadata = true;

    const auto result = engine.Convert(request);
    ASSERT_EQ(result.status, ConversionStatus::Success);
    EXPECT_FALSE(result.warnings.empty());
}

} // namespace
