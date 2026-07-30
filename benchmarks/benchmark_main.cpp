#include "nativeshift/core/conversion_engine.hpp"
#include "nativeshift/core/format_detector.hpp"
#include "nativeshift/core/job_queue.hpp"
#include "nativeshift/core/output_paths.hpp"
#include "nativeshift/image/image_provider.hpp"
#include "nativeshift/media/media_provider.hpp"
#include "nativeshift/platform/windows_platform_services.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using nativeshift::core::ConversionRequest;
using nativeshift::core::ConversionStatus;
using nativeshift::core::FileFormat;
using nativeshift::core::OutputConflictPolicy;

class TemporaryDirectory {
  public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("nativeshift-benchmark-" +
                 std::to_string(::GetCurrentProcessId()))) {
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

template <typename Operation>
double MeasureMilliseconds(Operation&& operation) {
    const auto started = Clock::now();
    operation();
    return std::chrono::duration<double, std::milli>(Clock::now() - started)
        .count();
}

void WriteLittleEndian(std::ofstream& output, const std::uint32_t value,
                       const std::size_t bytes) {
    for (std::size_t index = 0; index < bytes; ++index) {
        output.put(static_cast<char>((value >> (index * 8U)) & 0xFFU));
    }
}

void WriteBmp(const std::filesystem::path& path) {
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 64;
    constexpr std::uint32_t row_bytes = width * 3;
    constexpr std::uint32_t pixels = row_bytes * height;
    std::ofstream output(path, std::ios::binary);
    output.write("BM", 2);
    WriteLittleEndian(output, 54 + pixels, 4);
    WriteLittleEndian(output, 0, 4);
    WriteLittleEndian(output, 54, 4);
    WriteLittleEndian(output, 40, 4);
    WriteLittleEndian(output, width, 4);
    WriteLittleEndian(output, height, 4);
    WriteLittleEndian(output, 1, 2);
    WriteLittleEndian(output, 24, 2);
    WriteLittleEndian(output, 0, 4);
    WriteLittleEndian(output, pixels, 4);
    WriteLittleEndian(output, 2835, 4);
    WriteLittleEndian(output, 2835, 4);
    WriteLittleEndian(output, 0, 4);
    WriteLittleEndian(output, 0, 4);
    for (std::uint32_t index = 0; index < pixels; index += 3) {
        output.put(static_cast<char>(index % 255));
        output.put(static_cast<char>((index / 2) % 255));
        output.put(static_cast<char>((index / 3) % 255));
    }
}

void WriteWav(const std::filesystem::path& path) {
    constexpr std::uint32_t sample_rate = 48'000;
    constexpr std::uint16_t channels = 2;
    constexpr std::uint16_t bits = 16;
    constexpr std::uint32_t frames = sample_rate / 4;
    constexpr std::uint32_t data_bytes =
        frames * channels * (bits / 8);
    std::ofstream output(path, std::ios::binary);
    output.write("RIFF", 4);
    WriteLittleEndian(output, 36 + data_bytes, 4);
    output.write("WAVEfmt ", 8);
    WriteLittleEndian(output, 16, 4);
    WriteLittleEndian(output, 1, 2);
    WriteLittleEndian(output, channels, 2);
    WriteLittleEndian(output, sample_rate, 4);
    WriteLittleEndian(output, sample_rate * channels * (bits / 8), 4);
    WriteLittleEndian(output, channels * (bits / 8), 2);
    WriteLittleEndian(output, bits, 2);
    output.write("data", 4);
    WriteLittleEndian(output, data_bytes, 4);
    std::array<char, 4096> silence{};
    std::uint32_t remaining = data_bytes;
    while (remaining > 0) {
        const auto count =
            std::min(remaining,
                     static_cast<std::uint32_t>(silence.size()));
        output.write(silence.data(),
                     static_cast<std::streamsize>(count));
        remaining -= count;
    }
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

} // namespace

int wmain(const int argc, wchar_t* argv[]) {
    const bool include_media =
        argc > 1 && std::wstring_view(argv[1]) == L"--include-media";
    TemporaryDirectory directory;
    const auto bmp = directory.Path() / "input.bmp";
    const auto wav = directory.Path() / "input.wav";
    WriteBmp(bmp);
    WriteWav(wav);

    std::uint64_t detection_checksum = 0;
    const auto detection_ms = MeasureMilliseconds([&] {
        for (int index = 0; index < 10'000; ++index) {
            detection_checksum += static_cast<std::uint64_t>(
                nativeshift::core::DetectFormat(bmp).format);
        }
    });

    std::uint64_t naming_checksum = 0;
    const auto naming_ms = MeasureMilliseconds([&] {
        for (int index = 0; index < 100'000; ++index) {
            naming_checksum += nativeshift::core::BuildOutputPath(
                                   bmp, directory.Path(), FileFormat::Png)
                                   .native()
                                   .size();
        }
    });

    nativeshift::platform::WindowsPlatformServices platform;
    nativeshift::core::ConversionEngine engine(nullptr, &platform);
    engine.RegisterProvider(
        std::make_shared<nativeshift::image::ImageConversionProvider>());
    engine.RegisterProvider(
        std::make_shared<nativeshift::media::MediaConversionProvider>());

    const auto image_ms = MeasureMilliseconds([&] {
        for (int index = 0; index < 10; ++index) {
            const auto result = engine.Convert(Request(
                bmp,
                directory.Path() /
                    ("image-" + std::to_string(index) + ".png"),
                FileFormat::Png));
            if (result.status != ConversionStatus::Success) {
                std::cerr << "Image benchmark failed: " << result.message
                          << '\n';
            }
        }
    });

    const auto batch_ms = MeasureMilliseconds([&] {
        nativeshift::core::JobQueue queue(engine, 2, 32);
        std::vector<nativeshift::core::JobHandle> handles;
        for (int index = 0; index < 16; ++index) {
            auto handle = queue.TrySubmit(Request(
                bmp,
                directory.Path() /
                    ("batch-" + std::to_string(index) + ".png"),
                FileFormat::Png));
            if (handle.has_value()) {
                handles.push_back(std::move(*handle));
            }
        }
        for (const auto& handle : handles) {
            (void)handle.Get();
        }
    });

    double audio_ms = 0.0;
    if (include_media) {
        audio_ms = MeasureMilliseconds([&] {
            const auto result = engine.Convert(
                Request(wav, directory.Path() / "audio.flac",
                        FileFormat::Flac));
            if (result.status != ConversionStatus::Success) {
                std::cerr << "Audio benchmark failed: " << result.message
                          << '\n';
            }
        });
    }

    std::cout << "{\n"
              << "  \"format_detection_10000_ms\": " << detection_ms
              << ",\n"
              << "  \"output_naming_100000_ms\": " << naming_ms << ",\n"
              << "  \"image_bmp_to_png_10_ms\": " << image_ms << ",\n"
              << "  \"batch_image_16_jobs_ms\": " << batch_ms << ",\n"
              << "  \"audio_wav_to_flac_ms\": " << audio_ms << ",\n"
              << "  \"detection_checksum\": " << detection_checksum << ",\n"
              << "  \"naming_checksum\": " << naming_checksum << "\n"
              << "}\n";
    return 0;
}
