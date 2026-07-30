#include "nativeshift/image/image_provider.hpp"

#include "nativeshift/core/validation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <setjmp.h>
#include <string>
#include <utility>
#include <vector>

#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>
#include <webp/encode.h>

#ifdef _WIN32
#include <Windows.h>
#include <propidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace nativeshift::image {
namespace {

using nativeshift::core::ConversionProgress;
using nativeshift::core::ErrorCategory;
using nativeshift::core::FileFormat;
using nativeshift::core::ProgressCallback;
using nativeshift::core::ProviderOutcome;

struct PixelBuffer {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba;
};

struct FileCloser {
    void operator()(std::FILE* file) const noexcept {
        if (file != nullptr) {
            std::fclose(file);
        }
    }
};

using FilePointer = std::unique_ptr<std::FILE, FileCloser>;

std::filesystem::path ExtendedPath(const std::filesystem::path& path) {
#ifdef _WIN32
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }
    absolute.make_preferred();
    const auto native = absolute.native();
    if (native.starts_with(LR"(\\?\)")) {
        return absolute;
    }
    if (native.starts_with(LR"(\\)")) {
        return std::filesystem::path(std::wstring(LR"(\\?\UNC\)") +
                                     native.substr(2));
    }
    return std::filesystem::path(std::wstring(LR"(\\?\)") + native);
#else
    return path;
#endif
}

FilePointer OpenFile(const std::filesystem::path& path,
                     const wchar_t* windows_mode, const char* portable_mode) {
#ifdef _WIN32
    (void)portable_mode;
    std::FILE* raw = nullptr;
    const auto extended = ExtendedPath(path);
    if (_wfopen_s(&raw, extended.c_str(), windows_mode) != 0) {
        raw = nullptr;
    }
    return FilePointer(raw);
#else
    (void)windows_mode;
    return FilePointer(std::fopen(path.string().c_str(), portable_mode));
#endif
}

void Report(const ProgressCallback& progress, const double fraction,
            std::string stage) noexcept {
    if (!progress) {
        return;
    }
    try {
        progress(ConversionProgress{std::clamp(fraction, 0.0, 1.0),
                                    std::move(stage)});
    } catch (...) {
    }
}

bool CheckDimensions(const std::uint64_t width, const std::uint64_t height,
                     std::string& message) {
    if (width == 0 || height == 0 ||
        width > nativeshift::core::kMaximumImageDimension ||
        height > nativeshift::core::kMaximumImageDimension ||
        width * height > nativeshift::core::kMaximumImagePixels) {
        message =
            "Decoded image dimensions exceed the configured safety limits.";
        return false;
    }
    return true;
}

bool DecodePng(const std::filesystem::path& path, PixelBuffer& pixels,
               std::string& message, const std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        message = "Conversion was cancelled.";
        return false;
    }
    auto file = OpenFile(path, L"rb", "rb");
    if (!file) {
        message = "The PNG input could not be opened.";
        return false;
    }

    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_stdio(&image, file.get()) == 0) {
        message = std::string("The PNG header is invalid: ") + image.message;
        return false;
    }
    const auto cleanup = [&image] { png_image_free(&image); };
    if (!CheckDimensions(image.width, image.height, message)) {
        cleanup();
        return false;
    }

    image.format = PNG_FORMAT_RGBA;
    pixels.width = image.width;
    pixels.height = image.height;
    pixels.rgba.resize(PNG_IMAGE_SIZE(image));
    if (png_image_finish_read(&image, nullptr, pixels.rgba.data(), 0,
                              nullptr) == 0) {
        message =
            std::string("The PNG pixel data is invalid: ") + image.message;
        cleanup();
        return false;
    }
    cleanup();
    if (cancellation.stop_requested()) {
        message = "Conversion was cancelled.";
        return false;
    }
    return true;
}

struct JpegErrorManager {
    jpeg_error_mgr standard{};
    jmp_buf* jump{};
    std::array<char, JMSG_LENGTH_MAX> message{};
};

void JpegErrorExit(j_common_ptr common) {
    auto* error = reinterpret_cast<JpegErrorManager*>(common->err);
    (*common->err->format_message)(common, error->message.data());
    longjmp(*error->jump, 1);
}

struct JpegDecodeState {
    std::vector<std::uint8_t> row;
};

bool DecodeJpeg(const std::filesystem::path& path, PixelBuffer& pixels,
                std::string& message, const ProgressCallback& progress,
                const std::stop_token cancellation) {
    auto file = OpenFile(path, L"rb", "rb");
    if (!file) {
        message = "The JPEG input could not be opened.";
        return false;
    }

    jpeg_decompress_struct decoder{};
    JpegErrorManager error{};
    JpegDecodeState state;
    jmp_buf jump{};
    volatile bool created = false;
    decoder.err = jpeg_std_error(&error.standard);
    error.standard.error_exit = JpegErrorExit;
    error.jump = &jump;

#ifdef _MSC_VER
#pragma warning(push)
    // libjpeg's supported fatal-error contract requires setjmp/longjmp. All
    // owning C++ state is created before this checkpoint and remains in scope.
#pragma warning(disable : 4611)
#endif
    if (setjmp(jump) != 0) {
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        if (created) {
            jpeg_destroy_decompress(&decoder);
        }
        message =
            std::string("The JPEG data is invalid: ") + error.message.data();
        return false;
    }

    jpeg_create_decompress(&decoder);
    created = true;
    jpeg_stdio_src(&decoder, file.get());
    if (jpeg_read_header(&decoder, TRUE) != JPEG_HEADER_OK) {
        message = "The JPEG header is invalid.";
        jpeg_destroy_decompress(&decoder);
        return false;
    }
    if (!CheckDimensions(decoder.image_width, decoder.image_height, message)) {
        jpeg_destroy_decompress(&decoder);
        return false;
    }

    decoder.out_color_space = JCS_RGB;
    const bool cmyk = decoder.jpeg_color_space == JCS_CMYK ||
                      decoder.jpeg_color_space == JCS_YCCK;
    if (cmyk) {
        decoder.out_color_space = JCS_CMYK;
    }
    jpeg_start_decompress(&decoder);
    const int expected_components = cmyk ? 4 : 3;
    if (decoder.output_components != expected_components ||
        !CheckDimensions(decoder.output_width, decoder.output_height,
                         message)) {
        jpeg_destroy_decompress(&decoder);
        if (message.empty()) {
            message = "The JPEG color layout is unsupported.";
        }
        return false;
    }

    pixels.width = decoder.output_width;
    pixels.height = decoder.output_height;
    const auto pixel_count =
        static_cast<std::size_t>(pixels.width) * pixels.height;
    pixels.rgba.resize(pixel_count * 4);
    const auto component_count = static_cast<std::size_t>(expected_components);
    state.row.resize(static_cast<std::size_t>(pixels.width) * component_count);

    while (decoder.output_scanline < decoder.output_height) {
        if (cancellation.stop_requested()) {
            jpeg_abort_decompress(&decoder);
            jpeg_destroy_decompress(&decoder);
            message = "Conversion was cancelled.";
            return false;
        }
        JSAMPROW row_pointer = state.row.data();
        if (jpeg_read_scanlines(&decoder, &row_pointer, 1) != 1) {
            message = "The JPEG scanline data is incomplete.";
            jpeg_destroy_decompress(&decoder);
            return false;
        }
        const auto output_row =
            static_cast<std::size_t>(decoder.output_scanline - 1);
        auto* destination =
            pixels.rgba.data() +
            output_row * static_cast<std::size_t>(pixels.width) * 4;
        for (std::size_t x = 0; x < pixels.width; ++x) {
            if (cmyk) {
                const auto c = static_cast<unsigned int>(state.row[x * 4]);
                const auto m = static_cast<unsigned int>(state.row[x * 4 + 1]);
                const auto y = static_cast<unsigned int>(state.row[x * 4 + 2]);
                const auto k = static_cast<unsigned int>(state.row[x * 4 + 3]);
                if (decoder.saw_Adobe_marker != 0) {
                    destination[x * 4] =
                        static_cast<std::uint8_t>((c * k + 127U) / 255U);
                    destination[x * 4 + 1] =
                        static_cast<std::uint8_t>((m * k + 127U) / 255U);
                    destination[x * 4 + 2] =
                        static_cast<std::uint8_t>((y * k + 127U) / 255U);
                } else {
                    destination[x * 4] = static_cast<std::uint8_t>(
                        ((255U - c) * (255U - k) + 127U) / 255U);
                    destination[x * 4 + 1] = static_cast<std::uint8_t>(
                        ((255U - m) * (255U - k) + 127U) / 255U);
                    destination[x * 4 + 2] = static_cast<std::uint8_t>(
                        ((255U - y) * (255U - k) + 127U) / 255U);
                }
            } else {
                destination[x * 4] = state.row[x * 3];
                destination[x * 4 + 1] = state.row[x * 3 + 1];
                destination[x * 4 + 2] = state.row[x * 3 + 2];
            }
            destination[x * 4 + 3] = 255;
        }
        Report(progress,
               0.05 + 0.30 * (static_cast<double>(decoder.output_scanline) /
                              static_cast<double>(decoder.output_height)),
               "Decoding JPEG");
    }

    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);
    return true;
}

#ifdef _WIN32
class MappedFile {
  public:
    ~MappedFile() {
        if (data_ != nullptr) {
            ::UnmapViewOfFile(data_);
        }
        if (mapping_ != nullptr) {
            ::CloseHandle(mapping_);
        }
        if (file_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(file_);
        }
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile() = default;

    bool Open(const std::filesystem::path& path, std::string& message) {
        const auto extended = ExtendedPath(path);
        file_ = ::CreateFileW(extended.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            message = "The WebP input could not be opened.";
            return false;
        }
        LARGE_INTEGER size{};
        if (::GetFileSizeEx(file_, &size) == FALSE || size.QuadPart <= 0 ||
            static_cast<std::uint64_t>(size.QuadPart) >
                nativeshift::core::kMaximumEncodedImageBytes) {
            message =
                "The WebP input size is invalid or exceeds the safety limit.";
            return false;
        }
        size_ = static_cast<std::size_t>(size.QuadPart);
        mapping_ =
            ::CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping_ == nullptr) {
            message = "The WebP input could not be mapped.";
            return false;
        }
        data_ = ::MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
        if (data_ == nullptr) {
            message = "The WebP input mapping could not be read.";
            return false;
        }
        return true;
    }

    [[nodiscard]] const std::uint8_t* Data() const noexcept {
        return static_cast<const std::uint8_t*>(data_);
    }
    [[nodiscard]] std::size_t Size() const noexcept { return size_; }

  private:
    HANDLE file_{INVALID_HANDLE_VALUE};
    HANDLE mapping_{};
    void* data_{};
    std::size_t size_{};
};
#else
class MappedFile {
  public:
    bool Open(const std::filesystem::path& path, std::string& message) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) {
            message = "The WebP input could not be opened.";
            return false;
        }
        const auto size = input.tellg();
        if (size <= 0 || static_cast<std::uintmax_t>(size) >
                             nativeshift::core::kMaximumEncodedImageBytes) {
            message =
                "The WebP input size is invalid or exceeds the safety limit.";
            return false;
        }
        data_.resize(static_cast<std::size_t>(size));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(data_.data()),
                   static_cast<std::streamsize>(data_.size()));
        return static_cast<bool>(input);
    }
    [[nodiscard]] const std::uint8_t* Data() const noexcept {
        return data_.data();
    }
    [[nodiscard]] std::size_t Size() const noexcept { return data_.size(); }

  private:
    std::vector<std::uint8_t> data_;
};
#endif

bool DecodeWebP(const std::filesystem::path& path, PixelBuffer& pixels,
                std::string& message, const std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        message = "Conversion was cancelled.";
        return false;
    }
    MappedFile mapped;
    if (!mapped.Open(path, message)) {
        return false;
    }
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(mapped.Data(), mapped.Size(), &features) !=
        VP8_STATUS_OK) {
        message = "The WebP header is invalid.";
        return false;
    }
    if (features.has_animation != 0) {
        message = "Animated WebP input is not supported in Phase 1.";
        return false;
    }
    const int width = features.width;
    const int height = features.height;
    if (!CheckDimensions(static_cast<std::uint64_t>(std::max(width, 0)),
                         static_cast<std::uint64_t>(std::max(height, 0)),
                         message)) {
        if (message.empty()) {
            message = "The WebP header is invalid.";
        }
        return false;
    }
    pixels.width = static_cast<std::uint32_t>(width);
    pixels.height = static_cast<std::uint32_t>(height);
    const auto stride = static_cast<std::size_t>(pixels.width) * 4;
    pixels.rgba.resize(stride * pixels.height);
    if (WebPDecodeRGBAInto(mapped.Data(), mapped.Size(), pixels.rgba.data(),
                           pixels.rgba.size(),
                           static_cast<int>(stride)) == nullptr) {
        message = "The WebP pixel data is corrupt or unsupported.";
        return false;
    }
    if (cancellation.stop_requested()) {
        message = "Conversion was cancelled.";
        return false;
    }
    return true;
}

#ifdef _WIN32
class ComApartment {
  public:
    ComApartment() noexcept
        : result_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

    ~ComApartment() {
        if (result_ == S_OK || result_ == S_FALSE) {
            ::CoUninitialize();
        }
    }

    [[nodiscard]] bool Available() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

  private:
    HRESULT result_{};
};

using Microsoft::WRL::ComPtr;

bool CreateWicFactory(ComPtr<IWICImagingFactory>& factory,
                      std::string& message) {
    const HRESULT result =
        ::CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        message = "Windows Imaging Component could not be initialized.";
        return false;
    }
    return true;
}

bool DecodeWic(const std::filesystem::path& path, PixelBuffer& pixels,
               std::string& message, const std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        message = "Conversion was cancelled.";
        return false;
    }
    ComApartment apartment;
    if (!apartment.Available()) {
        message = "The Windows imaging apartment could not be initialized.";
        return false;
    }
    ComPtr<IWICImagingFactory> factory;
    if (!CreateWicFactory(factory, message)) {
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    const auto extended = ExtendedPath(path);
    if (FAILED(factory->CreateDecoderFromFilename(
            extended.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder))) {
        message = "The BMP/TIFF input could not be decoded by Windows.";
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        message = "The first BMP/TIFF image frame could not be read.";
        return false;
    }
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) ||
        !CheckDimensions(width, height, message)) {
        if (message.empty()) {
            message = "The BMP/TIFF dimensions could not be read.";
        }
        return false;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        message = "The BMP/TIFF pixel format is unsupported.";
        return false;
    }
    const auto stride64 = static_cast<std::uint64_t>(width) * 4ULL;
    const auto size64 = stride64 * static_cast<std::uint64_t>(height);
    if (stride64 > std::numeric_limits<UINT>::max() ||
        size64 > std::numeric_limits<UINT>::max()) {
        message = "The decoded BMP/TIFF buffer exceeds Windows API limits.";
        return false;
    }
    pixels.width = width;
    pixels.height = height;
    pixels.rgba.resize(static_cast<std::size_t>(size64));
    if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(stride64),
                                     static_cast<UINT>(size64),
                                     pixels.rgba.data()))) {
        message = "The BMP/TIFF pixel data is malformed or unsupported.";
        return false;
    }
    if (cancellation.stop_requested()) {
        message = "Conversion was cancelled.";
        return false;
    }
    return true;
}

std::uint16_t ReadExifOrientation(const std::filesystem::path& path) {
    ComApartment apartment;
    if (!apartment.Available()) {
        return 1;
    }
    ComPtr<IWICImagingFactory> factory;
    std::string ignored;
    if (!CreateWicFactory(factory, ignored)) {
        return 1;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    const auto extended = ExtendedPath(path);
    if (FAILED(factory->CreateDecoderFromFilename(
            extended.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder))) {
        return 1;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICMetadataQueryReader> metadata;
    if (FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(frame->GetMetadataQueryReader(&metadata))) {
        return 1;
    }
    constexpr std::array queries{
        L"/app1/ifd/{ushort=274}",
        L"/ifd/{ushort=274}",
    };
    for (const auto* query : queries) {
        PROPVARIANT value;
        ::PropVariantInit(&value);
        const HRESULT result = metadata->GetMetadataByName(query, &value);
        std::uint16_t orientation = 1;
        if (SUCCEEDED(result)) {
            if (value.vt == VT_UI2) {
                orientation = value.uiVal;
            } else if (value.vt == VT_UI4 &&
                       value.ulVal <=
                           std::numeric_limits<std::uint16_t>::max()) {
                orientation = static_cast<std::uint16_t>(value.ulVal);
            }
        }
        ::PropVariantClear(&value);
        if (orientation >= 1 && orientation <= 8) {
            return orientation;
        }
    }
    return 1;
}

bool EncodeWic(const std::filesystem::path& path,
               const FileFormat output_format, const PixelBuffer& pixels,
               const int compression_level, std::string& message,
               const std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        message = "Conversion was cancelled.";
        return false;
    }
    ComApartment apartment;
    if (!apartment.Available()) {
        message = "The Windows imaging apartment could not be initialized.";
        return false;
    }
    ComPtr<IWICImagingFactory> factory;
    if (!CreateWicFactory(factory, message)) {
        return false;
    }
    const GUID container = output_format == FileFormat::Bmp
                               ? GUID_ContainerFormatBmp
                               : GUID_ContainerFormatTiff;
    ComPtr<IWICStream> stream;
    const auto extended = ExtendedPath(path);
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(
            stream->InitializeFromFilename(extended.c_str(), GENERIC_WRITE))) {
        message = "The temporary BMP/TIFF output could not be opened.";
        return false;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(container, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        message = "The BMP/TIFF encoder could not be initialized.";
        return false;
    }
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (FAILED(encoder->CreateNewFrame(&frame, &properties))) {
        message = "The BMP/TIFF output frame could not be created.";
        return false;
    }
    if (output_format == FileFormat::Tiff && properties) {
        PROPBAG2 option{};
        option.pstrName = const_cast<wchar_t*>(L"TiffCompressionMethod");
        VARIANT value;
        ::VariantInit(&value);
        value.vt = VT_UI1;
        value.bVal =
            static_cast<BYTE>(compression_level == 0 ? WICTiffCompressionNone
                                                     : WICTiffCompressionZIP);
        if (FAILED(properties->Write(1, &option, &value))) {
            ::VariantClear(&value);
            message = "The TIFF compression option is unsupported.";
            return false;
        }
        ::VariantClear(&value);
    }
    if (FAILED(frame->Initialize(properties.Get())) ||
        FAILED(frame->SetSize(pixels.width, pixels.height))) {
        message = "The BMP/TIFF output frame could not be initialized.";
        return false;
    }
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&pixel_format))) {
        message = "The BMP/TIFF output pixel format is unsupported.";
        return false;
    }
    const auto stride64 = static_cast<std::uint64_t>(pixels.width) * 4ULL;
    const auto size64 = stride64 * pixels.height;
    if (stride64 > std::numeric_limits<UINT>::max() ||
        size64 > std::numeric_limits<UINT>::max()) {
        message = "The BMP/TIFF output exceeds Windows API limits.";
        return false;
    }
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromMemory(
            pixels.width, pixels.height, GUID_WICPixelFormat32bppRGBA,
            static_cast<UINT>(stride64), static_cast<UINT>(size64),
            const_cast<BYTE*>(pixels.rgba.data()), &bitmap))) {
        message = "The BMP/TIFF output bitmap could not be created.";
        return false;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(bitmap.Get(), pixel_format,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(frame->WriteSource(converter.Get(), nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        message = "BMP/TIFF encoding failed.";
        return false;
    }
    return !cancellation.stop_requested();
}
#endif

std::pair<std::uint32_t, std::uint32_t>
TargetDimensions(const PixelBuffer& source,
                 const nativeshift::core::ImageOptions& options) {
    if (!options.width && !options.height) {
        return {source.width, source.height};
    }

    const double source_width = static_cast<double>(source.width);
    const double source_height = static_cast<double>(source.height);
    double target_width =
        options.width ? static_cast<double>(*options.width) : source_width;
    double target_height =
        options.height ? static_cast<double>(*options.height) : source_height;

    if (options.resize_mode == nativeshift::core::ImageResizeMode::Fit) {
        if (options.width && options.height) {
            double scale = std::min(target_width / source_width,
                                    target_height / source_height);
            if (options.prevent_enlargement) {
                scale = std::min(scale, 1.0);
            }
            target_width = source_width * scale;
            target_height = source_height * scale;
        } else if (options.width) {
            if (options.prevent_enlargement) {
                target_width = std::min(target_width, source_width);
            }
            target_height = source_height * target_width / source_width;
        } else {
            if (options.prevent_enlargement) {
                target_height = std::min(target_height, source_height);
            }
            target_width = source_width * target_height / source_height;
        }
    } else if (options.resize_mode ==
               nativeshift::core::ImageResizeMode::Fill) {
        if (!options.width || !options.height) {
            const double scale = options.width ? target_width / source_width
                                               : target_height / source_height;
            const double bounded_scale =
                options.prevent_enlargement ? std::min(scale, 1.0) : scale;
            target_width = source_width * bounded_scale;
            target_height = source_height * bounded_scale;
        } else if (options.prevent_enlargement &&
                   (target_width > source_width ||
                    target_height > source_height)) {
            target_width = source_width;
            target_height = source_height;
        }
    } else if (options.prevent_enlargement) {
        target_width = std::min(target_width, source_width);
        target_height = std::min(target_height, source_height);
    }

    const auto width = static_cast<std::uint32_t>(
        std::clamp(std::llround(target_width), 1LL, 32'768LL));
    const auto height = static_cast<std::uint32_t>(
        std::clamp(std::llround(target_height), 1LL, 32'768LL));
    return {width, height};
}

bool Resize(PixelBuffer& pixels, const nativeshift::core::ImageOptions& options,
            const ProgressCallback& progress,
            const std::stop_token cancellation, std::string& message) {
    const auto [target_width, target_height] =
        TargetDimensions(pixels, options);
    if (target_width == pixels.width && target_height == pixels.height) {
        return true;
    }
    if (!CheckDimensions(target_width, target_height, message)) {
        return false;
    }

    PixelBuffer resized;
    resized.width = target_width;
    resized.height = target_height;
    resized.rgba.resize(static_cast<std::size_t>(target_width) * target_height *
                        4);

    double x_scale =
        static_cast<double>(pixels.width) / static_cast<double>(target_width);
    double y_scale =
        static_cast<double>(pixels.height) / static_cast<double>(target_height);
    double x_offset = 0.0;
    double y_offset = 0.0;
    if (options.resize_mode == nativeshift::core::ImageResizeMode::Fill &&
        options.width && options.height) {
        const double scale =
            std::max(static_cast<double>(target_width) / pixels.width,
                     static_cast<double>(target_height) / pixels.height);
        x_scale = 1.0 / scale;
        y_scale = 1.0 / scale;
        x_offset =
            (static_cast<double>(pixels.width) - target_width * x_scale) / 2.0;
        y_offset =
            (static_cast<double>(pixels.height) - target_height * y_scale) /
            2.0;
    }

    for (std::uint32_t y = 0; y < target_height; ++y) {
        if (cancellation.stop_requested()) {
            message = "Conversion was cancelled.";
            return false;
        }
        const double source_y = std::clamp(
            y_offset + (static_cast<double>(y) + 0.5) * y_scale - 0.5, 0.0,
            static_cast<double>(pixels.height - 1));
        const auto y0 = static_cast<std::uint32_t>(std::floor(source_y));
        const auto y1 = std::min(y0 + 1, pixels.height - 1);
        const double y_weight = source_y - static_cast<double>(y0);

        for (std::uint32_t x = 0; x < target_width; ++x) {
            const double source_x = std::clamp(
                x_offset + (static_cast<double>(x) + 0.5) * x_scale - 0.5, 0.0,
                static_cast<double>(pixels.width - 1));
            const auto x0 = static_cast<std::uint32_t>(std::floor(source_x));
            const auto x1 = std::min(x0 + 1, pixels.width - 1);
            const double x_weight = source_x - static_cast<double>(x0);
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const auto sample = [&pixels,
                                     channel](const std::uint32_t sample_x,
                                              const std::uint32_t sample_y) {
                    const auto index =
                        (static_cast<std::size_t>(sample_y) * pixels.width +
                         sample_x) *
                            4 +
                        channel;
                    return static_cast<double>(pixels.rgba[index]);
                };
                const double top = sample(x0, y0) * (1.0 - x_weight) +
                                   sample(x1, y0) * x_weight;
                const double bottom = sample(x0, y1) * (1.0 - x_weight) +
                                      sample(x1, y1) * x_weight;
                const double value = top * (1.0 - y_weight) + bottom * y_weight;
                const auto destination =
                    (static_cast<std::size_t>(y) * target_width + x) * 4 +
                    channel;
                resized.rgba[destination] = static_cast<std::uint8_t>(
                    std::clamp(std::llround(value), 0LL, 255LL));
            }
        }
        Report(progress,
               0.35 + 0.30 * (static_cast<double>(y + 1) /
                              static_cast<double>(target_height)),
               "Resizing image");
    }
    pixels = std::move(resized);
    return true;
}

bool ApplyOrientation(PixelBuffer& pixels, const std::uint16_t orientation,
                      const std::stop_token cancellation,
                      std::string& message) {
    if (orientation <= 1 || orientation > 8) {
        return true;
    }
    PixelBuffer transformed;
    const bool swaps_dimensions = orientation >= 5;
    transformed.width = swaps_dimensions ? pixels.height : pixels.width;
    transformed.height = swaps_dimensions ? pixels.width : pixels.height;
    transformed.rgba.resize(static_cast<std::size_t>(transformed.width) *
                            transformed.height * 4);

    for (std::uint32_t y = 0; y < transformed.height; ++y) {
        if (cancellation.stop_requested()) {
            message = "Conversion was cancelled.";
            return false;
        }
        for (std::uint32_t x = 0; x < transformed.width; ++x) {
            std::uint32_t source_x = x;
            std::uint32_t source_y = y;
            switch (orientation) {
            case 2:
                source_x = pixels.width - 1 - x;
                break;
            case 3:
                source_x = pixels.width - 1 - x;
                source_y = pixels.height - 1 - y;
                break;
            case 4:
                source_y = pixels.height - 1 - y;
                break;
            case 5:
                source_x = y;
                source_y = x;
                break;
            case 6:
                source_x = y;
                source_y = pixels.height - 1 - x;
                break;
            case 7:
                source_x = pixels.width - 1 - y;
                source_y = pixels.height - 1 - x;
                break;
            case 8:
                source_x = pixels.width - 1 - y;
                source_y = x;
                break;
            default:
                break;
            }
            const auto source_index =
                (static_cast<std::size_t>(source_y) * pixels.width + source_x) *
                4;
            const auto destination_index =
                (static_cast<std::size_t>(y) * transformed.width + x) * 4;
            std::copy_n(pixels.rgba.data() + source_index, 4,
                        transformed.rgba.data() + destination_index);
        }
    }
    pixels = std::move(transformed);
    return true;
}

void CompositeAlpha(PixelBuffer& pixels,
                    const nativeshift::core::RgbaColor background) {
    const std::array background_channels{
        background.red,
        background.green,
        background.blue,
    };
    for (std::size_t index = 0; index < pixels.rgba.size(); index += 4) {
        const auto alpha = static_cast<unsigned int>(pixels.rgba[index + 3]);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto color =
                static_cast<unsigned int>(pixels.rgba[index + channel]);
            const auto backdrop =
                static_cast<unsigned int>(background_channels[channel]);
            pixels.rgba[index + channel] = static_cast<std::uint8_t>(
                (color * alpha + backdrop * (255U - alpha) + 127U) / 255U);
        }
        pixels.rgba[index + 3] = 255;
    }
}

bool EncodePng(const std::filesystem::path& path, const PixelBuffer& pixels,
               std::string& message, const std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        message = "Conversion was cancelled.";
        return false;
    }
    auto file = OpenFile(path, L"wb", "wb");
    if (!file) {
        message = "The temporary PNG output could not be opened.";
        return false;
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = pixels.width;
    image.height = pixels.height;
    image.format = PNG_FORMAT_RGBA;
    const auto stride =
        static_cast<png_int_32>(static_cast<std::uint64_t>(pixels.width) * 4);
    if (png_image_write_to_stdio(&image, file.get(), 0, pixels.rgba.data(),
                                 stride, nullptr) == 0) {
        message = std::string("PNG encoding failed: ") + image.message;
        return false;
    }
    return true;
}

struct JpegEncodeState {
    std::vector<std::uint8_t> row;
};

bool EncodeJpeg(const std::filesystem::path& path, const PixelBuffer& pixels,
                const int quality, std::string& message,
                const ProgressCallback& progress,
                const std::stop_token cancellation) {
    auto file = OpenFile(path, L"wb", "wb");
    if (!file) {
        message = "The temporary JPEG output could not be opened.";
        return false;
    }

    jpeg_compress_struct encoder{};
    JpegErrorManager error{};
    JpegEncodeState state;
    jmp_buf jump{};
    volatile bool created = false;
    encoder.err = jpeg_std_error(&error.standard);
    error.standard.error_exit = JpegErrorExit;
    error.jump = &jump;

#ifdef _MSC_VER
#pragma warning(push)
    // See DecodeJpeg: this is libjpeg's required recoverable error boundary.
#pragma warning(disable : 4611)
#endif
    if (setjmp(jump) != 0) {
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        if (created) {
            jpeg_destroy_compress(&encoder);
        }
        message = std::string("JPEG encoding failed: ") + error.message.data();
        return false;
    }

    jpeg_create_compress(&encoder);
    created = true;
    jpeg_stdio_dest(&encoder, file.get());
    encoder.image_width = pixels.width;
    encoder.image_height = pixels.height;
    encoder.input_components = 3;
    encoder.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder);
    jpeg_set_quality(&encoder, quality, TRUE);
    jpeg_start_compress(&encoder, TRUE);

    state.row.resize(static_cast<std::size_t>(pixels.width) * 3);
    while (encoder.next_scanline < encoder.image_height) {
        if (cancellation.stop_requested()) {
            jpeg_abort_compress(&encoder);
            jpeg_destroy_compress(&encoder);
            message = "Conversion was cancelled.";
            return false;
        }
        const auto y = static_cast<std::size_t>(encoder.next_scanline);
        const auto* source =
            pixels.rgba.data() + y * static_cast<std::size_t>(pixels.width) * 4;
        for (std::size_t x = 0; x < pixels.width; ++x) {
            const auto alpha = static_cast<unsigned int>(source[x * 4 + 3]);
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto color =
                    static_cast<unsigned int>(source[x * 4 + channel]);
                state.row[x * 3 + channel] = static_cast<std::uint8_t>(
                    (color * alpha + 255U * (255U - alpha) + 127U) / 255U);
            }
        }
        JSAMPROW row_pointer = state.row.data();
        jpeg_write_scanlines(&encoder, &row_pointer, 1);
        Report(progress,
               0.65 + 0.33 * (static_cast<double>(encoder.next_scanline) /
                              static_cast<double>(encoder.image_height)),
               "Encoding JPEG");
    }
    jpeg_finish_compress(&encoder);
    jpeg_destroy_compress(&encoder);
    return true;
}

struct WebPWriteContext {
    std::FILE* file{};
    std::stop_token cancellation;
    const ProgressCallback* progress{};
    bool write_failed{false};
};

int WebPWrite(const std::uint8_t* data, const std::size_t size,
              const WebPPicture* picture) {
    auto* context = static_cast<WebPWriteContext*>(picture->custom_ptr);
    if (context->cancellation.stop_requested()) {
        return 0;
    }
    if (std::fwrite(data, 1, size, context->file) != size) {
        context->write_failed = true;
        return 0;
    }
    return 1;
}

int WebPProgress(const int percent, const WebPPicture* picture) {
    auto* context = static_cast<WebPWriteContext*>(picture->user_data);
    if (context->cancellation.stop_requested()) {
        return 0;
    }
    if (context->progress != nullptr) {
        Report(*context->progress,
               0.65 + 0.33 * (static_cast<double>(percent) / 100.0),
               "Encoding WebP");
    }
    return 1;
}

bool EncodeWebP(const std::filesystem::path& path, const PixelBuffer& pixels,
                const int quality, const bool lossless, std::string& message,
                const ProgressCallback& progress,
                const std::stop_token cancellation) {
    auto file = OpenFile(path, L"wb", "wb");
    if (!file) {
        message = "The temporary WebP output could not be opened.";
        return false;
    }

    WebPConfig config{};
    if (WebPConfigPreset(&config, WEBP_PRESET_DEFAULT,
                         static_cast<float>(quality)) == 0 ||
        WebPValidateConfig(&config) == 0) {
        message = "The WebP encoder configuration is invalid.";
        return false;
    }
    config.lossless = lossless ? 1 : 0;
    if (lossless) {
        config.quality = 100.0F;
    }
    config.method = 4;
    config.thread_level = 1;

    WebPPicture picture{};
    if (WebPPictureInit(&picture) == 0) {
        message = "The WebP encoder could not be initialized.";
        return false;
    }
    picture.use_argb = 1;
    picture.width = static_cast<int>(pixels.width);
    picture.height = static_cast<int>(pixels.height);
    WebPWriteContext context{file.get(), cancellation, &progress, false};
    picture.writer = WebPWrite;
    picture.custom_ptr = &context;
    picture.progress_hook = WebPProgress;
    picture.user_data = &context;

    const auto stride =
        static_cast<int>(static_cast<std::uint64_t>(pixels.width) * 4);
    if (WebPPictureImportRGBA(&picture, pixels.rgba.data(), stride) == 0) {
        WebPPictureFree(&picture);
        message = "The WebP encoder could not import the pixel buffer.";
        return false;
    }
    const int encoded = WebPEncode(&config, &picture);
    WebPPictureFree(&picture);
    if (encoded == 0) {
        if (cancellation.stop_requested()) {
            message = "Conversion was cancelled.";
        } else if (context.write_failed) {
            message = "The WebP output could not be written.";
        } else {
            message = "WebP encoding failed.";
        }
        return false;
    }
    return true;
}

} // namespace

std::string ImageConversionProvider::Name() const {
    return "NativeImageCodecs";
}

bool ImageConversionProvider::CanHandle(
    const FileFormat input, const FileFormat output) const noexcept {
    return nativeshift::core::IsImageFormat(input) &&
           nativeshift::core::IsImageFormat(output);
}

std::vector<nativeshift::core::ValidationIssue>
ImageConversionProvider::Validate(
    const nativeshift::core::ConversionRequest& request) const {
    std::vector<nativeshift::core::ValidationIssue> issues;
    if (!CanHandle(request.input_format, request.output_format)) {
        issues.push_back(
            {ErrorCategory::UnsupportedFormat, "unsupported_image_pair",
             "The image provider supports PNG, JPEG, WebP, BMP, and TIFF."});
    }
    if (request.image.rotation_degrees != 0 &&
        request.image.rotation_degrees != 90 &&
        request.image.rotation_degrees != 180 &&
        request.image.rotation_degrees != 270) {
        issues.push_back(
            {ErrorCategory::InvalidRequest, "rotation",
             "Image rotation must be 0, 90, 180, or 270 degrees."});
    }
    if (request.image.compression_level < 0 ||
        request.image.compression_level > 9) {
        issues.push_back({ErrorCategory::InvalidRequest, "compression_level",
                          "Image compression level must be between 0 and 9."});
    }
    return issues;
}

std::uintmax_t ImageConversionProvider::EstimateOutput(
    const nativeshift::core::ConversionRequest& request) const {
    if (request.image.width && request.image.height) {
        return static_cast<std::uintmax_t>(*request.image.width) *
                   static_cast<std::uintmax_t>(*request.image.height) * 4ULL +
               1024ULL * 1024ULL;
    }
    std::error_code error;
    const auto input_size =
        std::filesystem::file_size(request.input_path, error);
    if (error || input_size > std::numeric_limits<std::uintmax_t>::max() / 4) {
        return 512ULL * 1024ULL * 1024ULL;
    }
    return std::max<std::uintmax_t>(input_size * 4, 16ULL * 1024ULL * 1024ULL);
}

ProviderOutcome ImageConversionProvider::Convert(
    const nativeshift::core::ConversionRequest& request,
    const ProgressCallback& progress, const std::stop_token cancellation) {
    PixelBuffer pixels;
    std::string message;
    Report(progress, 0.05, "Decoding image");

    bool decoded = false;
    switch (request.input_format) {
    case FileFormat::Png:
        decoded = DecodePng(request.input_path, pixels, message, cancellation);
        break;
    case FileFormat::Jpeg:
        decoded = DecodeJpeg(request.input_path, pixels, message, progress,
                             cancellation);
        break;
    case FileFormat::WebP:
        decoded = DecodeWebP(request.input_path, pixels, message, cancellation);
        break;
    case FileFormat::Bmp:
    case FileFormat::Tiff:
#ifdef _WIN32
        decoded = DecodeWic(request.input_path, pixels, message, cancellation);
#else
        message = "BMP/TIFF conversion requires Windows Imaging Component.";
#endif
        break;
    default:
        return ProviderOutcome::Failed(
            ErrorCategory::UnsupportedFormat,
            "The image input format is unsupported.");
    }
    if (!decoded) {
        return cancellation.stop_requested()
                   ? ProviderOutcome::Cancelled(std::move(message))
                   : ProviderOutcome::Failed(ErrorCategory::InvalidInput,
                                             std::move(message));
    }
    Report(progress, 0.35, "Image decoded");

    if (request.image.automatic_orientation) {
#ifdef _WIN32
        const auto orientation = ReadExifOrientation(request.input_path);
        if (!ApplyOrientation(pixels, orientation, cancellation, message)) {
            return ProviderOutcome::Cancelled(std::move(message));
        }
#endif
    }
    std::uint16_t requested_orientation = 1;
    if (request.image.rotation_degrees == 90) {
        requested_orientation = 6;
    } else if (request.image.rotation_degrees == 180) {
        requested_orientation = 3;
    } else if (request.image.rotation_degrees == 270) {
        requested_orientation = 8;
    }
    if (!ApplyOrientation(pixels, requested_orientation, cancellation,
                          message)) {
        return ProviderOutcome::Cancelled(std::move(message));
    }

    if (!Resize(pixels, request.image, progress, cancellation, message)) {
        return cancellation.stop_requested()
                   ? ProviderOutcome::Cancelled(std::move(message))
                   : ProviderOutcome::Failed(ErrorCategory::ResourceLimit,
                                             std::move(message));
    }
    if (cancellation.stop_requested()) {
        return ProviderOutcome::Cancelled("Conversion was cancelled.");
    }

    Report(progress, 0.65, "Encoding image");
    if (request.output_format == FileFormat::Jpeg ||
        request.output_format == FileFormat::Bmp) {
        CompositeAlpha(pixels, request.image.background);
    }
    bool encoded = false;
    switch (request.output_format) {
    case FileFormat::Png:
        encoded = EncodePng(request.output_path, pixels, message, cancellation);
        break;
    case FileFormat::Jpeg:
        encoded = EncodeJpeg(request.output_path, pixels, request.image.quality,
                             message, progress, cancellation);
        break;
    case FileFormat::WebP:
        encoded =
            EncodeWebP(request.output_path, pixels, request.image.quality,
                       request.image.lossless, message, progress, cancellation);
        break;
    case FileFormat::Bmp:
    case FileFormat::Tiff:
#ifdef _WIN32
        encoded =
            EncodeWic(request.output_path, request.output_format, pixels,
                      request.image.compression_level, message, cancellation);
#else
        message = "BMP/TIFF conversion requires Windows Imaging Component.";
#endif
        break;
    default:
        return ProviderOutcome::Failed(
            ErrorCategory::UnsupportedFormat,
            "The image output format is unsupported.");
    }

    if (!encoded) {
        return cancellation.stop_requested()
                   ? ProviderOutcome::Cancelled(std::move(message))
                   : ProviderOutcome::Failed(ErrorCategory::Codec,
                                             std::move(message));
    }

    auto outcome = ProviderOutcome::Succeeded();
    if (request.image.preserve_metadata) {
        outcome.warnings.push_back(
            "This conversion path cannot preserve all metadata; metadata was "
            "removed. Disable preservation or choose a metadata-capable "
            "provider.");
    }
    if (request.image.preserve_color_profile) {
        outcome.warnings.push_back(
            "Colour-profile preservation is unavailable for this conversion "
            "path; pixels were converted to sRGB-compatible RGBA.");
    }
    return outcome;
}

std::vector<nativeshift::core::FormatPair>
ImageConversionProvider::GetSupportedFormats() const {
    std::vector<nativeshift::core::FormatPair> pairs;
    constexpr std::array formats{
        FileFormat::Png, FileFormat::Jpeg, FileFormat::WebP,
        FileFormat::Bmp, FileFormat::Tiff,
    };
    for (const auto input : formats) {
        for (const auto output : formats) {
            pairs.push_back({input, output});
        }
    }
    return pairs;
}

std::vector<std::string> ImageConversionProvider::GetAvailableOptions() const {
    return {
        "quality",
        "lossless",
        "width",
        "height",
        "fit",
        "fill",
        "stretch",
        "prevent-enlargement",
        "rotation",
        "automatic-orientation",
        "preserve-metadata",
        "preserve-colour-profile",
        "background-colour",
        "compression-level",
    };
}

} // namespace nativeshift::image
