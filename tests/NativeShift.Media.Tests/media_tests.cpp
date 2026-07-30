#include "nativeshift/core/conversion_engine.hpp"
#include "nativeshift/core/format_detector.hpp"
#include "nativeshift/media/media_provider.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stop_token>
#include <string>

#include <gtest/gtest.h>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

using nativeshift::core::ConversionEngine;
using nativeshift::core::ConversionRequest;
using nativeshift::core::ConversionStatus;
using nativeshift::core::FileFormat;
using nativeshift::core::OutputConflictPolicy;

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{1};
        path_ = std::filesystem::temp_directory_path() /
                ("nativeshift-media-tests-" +
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

void WriteU16(std::ofstream& output, const std::uint16_t value) {
    output.put(static_cast<char>(value & 0xFFU));
    output.put(static_cast<char>((value >> 8U) & 0xFFU));
}

void WriteU32(std::ofstream& output, const std::uint32_t value) {
    output.put(static_cast<char>(value & 0xFFU));
    output.put(static_cast<char>((value >> 8U) & 0xFFU));
    output.put(static_cast<char>((value >> 16U) & 0xFFU));
    output.put(static_cast<char>((value >> 24U) & 0xFFU));
}

void WriteTestWav(const std::filesystem::path& path,
                  const int sample_rate = 44'100, const int channels = 2) {
    constexpr int duration_samples = 4'410;
    constexpr int bits_per_sample = 16;
    const auto data_size = static_cast<std::uint32_t>(
        duration_samples * channels * (bits_per_sample / 8));
    std::ofstream output(path, std::ios::binary);
    output.write("RIFF", 4);
    WriteU32(output, 36U + data_size);
    output.write("WAVEfmt ", 8);
    WriteU32(output, 16);
    WriteU16(output, 1);
    WriteU16(output, static_cast<std::uint16_t>(channels));
    WriteU32(output, static_cast<std::uint32_t>(sample_rate));
    WriteU32(output, static_cast<std::uint32_t>(sample_rate * channels *
                                                (bits_per_sample / 8)));
    WriteU16(output,
             static_cast<std::uint16_t>(channels * (bits_per_sample / 8)));
    WriteU16(output, bits_per_sample);
    output.write("data", 4);
    WriteU32(output, data_size);
    for (int sample = 0; sample < duration_samples; ++sample) {
        const auto value = static_cast<std::int16_t>(
            std::sin(static_cast<double>(sample) * 0.062689377) * 8'000.0);
        for (int channel = 0; channel < channels; ++channel) {
            WriteU16(output, static_cast<std::uint16_t>(value));
        }
    }
}

ConversionEngine MakeEngine() {
    ConversionEngine engine;
    engine.RegisterProvider(
        std::make_shared<nativeshift::media::MediaConversionProvider>());
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

struct AudioProbe {
    int sample_rate{};
    int channels{};
    AVCodecID codec{AV_CODEC_ID_NONE};
};

AudioProbe ProbeAudio(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    const std::string name(reinterpret_cast<const char*>(utf8.data()),
                           utf8.size());
    AVFormatContext* context = nullptr;
    if (avformat_open_input(&context, name.c_str(), nullptr, nullptr) < 0) {
        return {};
    }
    const std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> owner(
        context, [](AVFormatContext* value) { avformat_close_input(&value); });
    if (avformat_find_stream_info(context, nullptr) < 0) {
        return {};
    }
    const int stream =
        av_find_best_stream(context, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (stream < 0) {
        return {};
    }
    const auto* parameters = context->streams[stream]->codecpar;
    return {parameters->sample_rate, parameters->ch_layout.nb_channels,
            parameters->codec_id};
}

bool WriteTestVideo(const std::filesystem::path& path,
                    const bool with_audio = false) {
    const auto utf8 = path.u8string();
    const std::string name(reinterpret_cast<const char*>(utf8.data()),
                           utf8.size());
    AVFormatContext* output = nullptr;
    AVCodecContext* encoder_context = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    const auto cleanup = [&] {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&encoder_context);
        if (output != nullptr) {
            if (output->pb != nullptr &&
                (output->oformat->flags & AVFMT_NOFILE) == 0) {
                avio_closep(&output->pb);
            }
            avformat_free_context(output);
        }
    };

    if (avformat_alloc_output_context2(&output, nullptr, "matroska",
                                       name.c_str()) < 0 ||
        output == nullptr) {
        cleanup();
        return false;
    }
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_FFV1);
    if (encoder == nullptr) {
        cleanup();
        return false;
    }
    AVStream* stream = avformat_new_stream(output, nullptr);
    encoder_context = avcodec_alloc_context3(encoder);
    if (stream == nullptr || encoder_context == nullptr) {
        cleanup();
        return false;
    }
    encoder_context->width = 64;
    encoder_context->height = 48;
    encoder_context->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder_context->time_base = {1, 10};
    encoder_context->framerate = {10, 1};
    if ((output->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        encoder_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (avcodec_open2(encoder_context, encoder, nullptr) < 0 ||
        avcodec_parameters_from_context(stream->codecpar, encoder_context) <
            0) {
        cleanup();
        return false;
    }
    stream->time_base = encoder_context->time_base;
    AVStream* audio_stream = nullptr;
    if (with_audio) {
        audio_stream = avformat_new_stream(output, nullptr);
        if (audio_stream == nullptr) {
            cleanup();
            return false;
        }
        audio_stream->time_base = {1, 8'000};
        audio_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        audio_stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
        audio_stream->codecpar->format = AV_SAMPLE_FMT_S16;
        audio_stream->codecpar->sample_rate = 8'000;
        av_channel_layout_default(&audio_stream->codecpar->ch_layout, 1);
        audio_stream->codecpar->bits_per_coded_sample = 16;
        audio_stream->codecpar->bits_per_raw_sample = 16;
        audio_stream->codecpar->block_align = 2;
        audio_stream->codecpar->bit_rate = 128'000;
    }
    if (((output->oformat->flags & AVFMT_NOFILE) == 0 &&
         avio_open(&output->pb, name.c_str(), AVIO_FLAG_WRITE) < 0) ||
        avformat_write_header(output, nullptr) < 0) {
        cleanup();
        return false;
    }

    frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (frame == nullptr || packet == nullptr) {
        cleanup();
        return false;
    }
    frame->format = encoder_context->pix_fmt;
    frame->width = encoder_context->width;
    frame->height = encoder_context->height;
    if (av_frame_get_buffer(frame, 32) < 0) {
        cleanup();
        return false;
    }

    const auto drain = [&]() {
        while (true) {
            const int status = avcodec_receive_packet(encoder_context, packet);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return true;
            }
            if (status < 0) {
                return false;
            }
            av_packet_rescale_ts(packet, encoder_context->time_base,
                                 stream->time_base);
            packet->stream_index = stream->index;
            const int write_status = av_interleaved_write_frame(output, packet);
            av_packet_unref(packet);
            if (write_status < 0) {
                return false;
            }
        }
    };

    for (int index = 0; index < 6; ++index) {
        if (av_frame_make_writable(frame) < 0) {
            cleanup();
            return false;
        }
        for (int row = 0; row < frame->height; ++row) {
            std::fill_n(frame->data[0] + row * frame->linesize[0], frame->width,
                        static_cast<std::uint8_t>(40 + index * 20));
        }
        for (int row = 0; row < frame->height / 2; ++row) {
            std::fill_n(frame->data[1] + row * frame->linesize[1],
                        frame->width / 2, static_cast<std::uint8_t>(90));
            std::fill_n(frame->data[2] + row * frame->linesize[2],
                        frame->width / 2, static_cast<std::uint8_t>(160));
        }
        frame->pts = index;
        if (avcodec_send_frame(encoder_context, frame) < 0 || !drain()) {
            cleanup();
            return false;
        }
    }
    if (avcodec_send_frame(encoder_context, nullptr) < 0 || !drain()) {
        cleanup();
        return false;
    }
    if (audio_stream != nullptr) {
        constexpr int audio_samples = 4'800;
        if (av_new_packet(packet, audio_samples * 2) < 0) {
            cleanup();
            return false;
        }
        for (int sample = 0; sample < audio_samples; ++sample) {
            const auto value = static_cast<std::int16_t>(
                std::sin(static_cast<double>(sample) * 0.125663706) * 6'000.0);
            packet->data[sample * 2] = static_cast<std::uint8_t>(value & 0xFF);
            packet->data[sample * 2 + 1] = static_cast<std::uint8_t>(
                (static_cast<std::uint16_t>(value) >> 8U) & 0xFFU);
        }
        packet->pts = 0;
        packet->dts = 0;
        packet->duration = audio_samples;
        packet->stream_index = audio_stream->index;
        packet->flags = AV_PKT_FLAG_KEY;
        const int write_status = av_interleaved_write_frame(output, packet);
        av_packet_unref(packet);
        if (write_status < 0) {
            cleanup();
            return false;
        }
    }
    if (av_write_trailer(output) < 0) {
        cleanup();
        return false;
    }
    cleanup();
    return true;
}

TEST(AudioConversion, ConvertsWavToFlacUsingNativeLibraries) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "source.wav";
    const auto output = directory.Path() / "result.flac";
    WriteTestWav(input);

    auto engine = MakeEngine();
    const auto result =
        engine.Convert(Request(input, output, FileFormat::Flac));

    ASSERT_EQ(result.status, ConversionStatus::Success) << result.message;
    EXPECT_EQ(nativeshift::core::DetectFormat(output).format, FileFormat::Flac);
}

TEST(AudioConversion, ConvertsWavToMp3AndBackToWav) {
    TemporaryDirectory directory;
    const auto source = directory.Path() / "source.wav";
    const auto mp3 = directory.Path() / "encoded.mp3";
    const auto decoded = directory.Path() / "decoded.wav";
    WriteTestWav(source);

    auto engine = MakeEngine();
    ASSERT_EQ(engine.Convert(Request(source, mp3, FileFormat::Mp3)).status,
              ConversionStatus::Success);
    const auto result = engine.Convert(Request(mp3, decoded, FileFormat::Wav));

    ASSERT_EQ(result.status, ConversionStatus::Success) << result.message;
    EXPECT_EQ(nativeshift::core::DetectFormat(decoded).format, FileFormat::Wav);
}

TEST(AudioConversion, ResamplesAndDownmixes) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "source.wav";
    const auto output = directory.Path() / "mono.flac";
    WriteTestWav(input);
    auto request = Request(input, output, FileFormat::Flac);
    request.audio.sample_rate = 22'050;
    request.audio.channels = 1;
    request.audio.preserve_source_settings = false;

    auto engine = MakeEngine();
    const auto result = engine.Convert(std::move(request));
    const auto probe = ProbeAudio(output);

    ASSERT_EQ(result.status, ConversionStatus::Success) << result.message;
    EXPECT_EQ(probe.sample_rate, 22'050);
    EXPECT_EQ(probe.channels, 1);
}

TEST(AudioConversion, RejectsInvalidInputAndRemovesTemporaryOutput) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "invalid.wav";
    const auto output = directory.Path() / "result.flac";
    std::ofstream(input, std::ios::binary) << "not media";

    auto engine = MakeEngine();
    const auto result =
        engine.Convert(Request(input, output, FileFormat::Flac));

    EXPECT_EQ(result.status, ConversionStatus::Failed);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(MediaValidation, RejectsIncompatibleCodecAndContainer) {
    nativeshift::media::MediaConversionProvider provider;
    ConversionRequest request;
    request.input_format = FileFormat::Mp4;
    request.output_format = FileFormat::WebM;
    request.video.video_codec = nativeshift::core::VideoCodec::H264;

    const auto issues = provider.Validate(request);
    EXPECT_FALSE(issues.empty());
    EXPECT_EQ(issues.front().code, "video.codec_container");
}

TEST(VideoConversion, EncodesVp9WebmWithSoftwareFallback) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "source.mkv";
    const auto output = directory.Path() / "result.webm";
    ASSERT_TRUE(WriteTestVideo(input));
    auto request = Request(input, output, FileFormat::WebM);
    request.video.video_codec = nativeshift::core::VideoCodec::Vp9;
    request.video.audio_codec = nativeshift::core::VideoAudioCodec::None;
    request.video.hardware_acceleration =
        nativeshift::core::HardwareAcceleration::SoftwareOnly;
    request.video.width = 32;

    auto engine = MakeEngine();
    const auto result = engine.Convert(std::move(request));

    ASSERT_EQ(result.status, ConversionStatus::Success) << result.message;
    EXPECT_EQ(nativeshift::core::DetectFormat(output).format, FileFormat::WebM);
    EXPECT_EQ(result.selected_codec, "libvpx-vp9");
    EXPECT_EQ(result.hardware_acceleration, "software");
}

TEST(VideoConversion, TranscodesAudioIntoRequestedVideoContainer) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "source-with-audio.mkv";
    const auto output = directory.Path() / "result-with-audio.webm";
    ASSERT_TRUE(WriteTestVideo(input, true));
    auto request = Request(input, output, FileFormat::WebM);
    request.video.video_codec = nativeshift::core::VideoCodec::Vp9;
    request.video.audio_codec = nativeshift::core::VideoAudioCodec::Opus;
    request.video.hardware_acceleration =
        nativeshift::core::HardwareAcceleration::SoftwareOnly;
    request.video.audio_sample_rate = 24'000;
    request.video.audio_channels = 1;

    auto engine = MakeEngine();
    const auto result = engine.Convert(std::move(request));
    const auto audio = ProbeAudio(output);

    ASSERT_EQ(result.status, ConversionStatus::Success) << result.message;
    EXPECT_EQ(audio.codec, AV_CODEC_ID_OPUS);
    EXPECT_EQ(audio.channels, 1);
    EXPECT_TRUE(
        std::ranges::any_of(result.warnings, [](const std::string& warning) {
            return warning.find("Audio transcoded with") != std::string::npos;
        }));
}

TEST(VideoConversion, RemuxesCompatibleStreamsWithoutReencoding) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "source.mkv";
    const auto output = directory.Path() / "copy.mkv";
    ASSERT_TRUE(WriteTestVideo(input));
    auto request = Request(input, output, FileFormat::Mkv);
    request.video.stream_copy = true;
    request.video.video_codec = nativeshift::core::VideoCodec::Copy;
    request.video.audio_codec = nativeshift::core::VideoAudioCodec::None;

    auto engine = MakeEngine();
    const auto result = engine.Convert(std::move(request));

    ASSERT_EQ(result.status, ConversionStatus::Success) << result.message;
    EXPECT_EQ(nativeshift::core::DetectFormat(output).format, FileFormat::Mkv);
    EXPECT_EQ(result.selected_codec, "stream-copy");
}

TEST(MediaConversion, HonorsCancellationBeforeStarting) {
    TemporaryDirectory directory;
    const auto input = directory.Path() / "source.wav";
    const auto output = directory.Path() / "cancelled.flac";
    WriteTestWav(input);
    std::stop_source cancellation;
    cancellation.request_stop();

    auto engine = MakeEngine();
    const auto result = engine.Convert(Request(input, output, FileFormat::Flac),
                                       {}, cancellation.get_token());

    EXPECT_EQ(result.status, ConversionStatus::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(output));
}

} // namespace
