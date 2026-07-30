#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "nativeshift/core/formats.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <string>

namespace nativeshift::media::detail {

struct CodecContextDeleter {
    void operator()(AVCodecContext* value) const noexcept {
        avcodec_free_context(&value);
    }
};

struct FrameDeleter {
    void operator()(AVFrame* value) const noexcept { av_frame_free(&value); }
};

struct PacketDeleter {
    void operator()(AVPacket* value) const noexcept { av_packet_free(&value); }
};

struct SwrDeleter {
    void operator()(SwrContext* value) const noexcept { swr_free(&value); }
};

struct SwsDeleter {
    void operator()(SwsContext* value) const noexcept {
        sws_freeContext(value);
    }
};

struct AudioFifoDeleter {
    void operator()(AVAudioFifo* value) const noexcept {
        av_audio_fifo_free(value);
    }
};

using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
using AudioFifoPtr = std::unique_ptr<AVAudioFifo, AudioFifoDeleter>;

class InputContext {
  public:
    ~InputContext() {
        if (value != nullptr) {
            avformat_close_input(&value);
        }
    }

    InputContext(const InputContext&) = delete;
    InputContext& operator=(const InputContext&) = delete;
    InputContext() = default;

    AVFormatContext* value{};
};

class OutputContext {
  public:
    ~OutputContext() {
        if (value != nullptr) {
            if (value->pb != nullptr &&
                (value->oformat->flags & AVFMT_NOFILE) == 0) {
                avio_closep(&value->pb);
            }
            avformat_free_context(value);
        }
    }

    OutputContext(const OutputContext&) = delete;
    OutputContext& operator=(const OutputContext&) = delete;
    OutputContext() = default;

    AVFormatContext* value{};
};

[[nodiscard]] inline std::string ErrorText(const int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
        return "FFmpeg error " + std::to_string(code);
    }
    return buffer.data();
}

[[nodiscard]] inline std::string PathToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] inline const char*
MuxerName(const core::FileFormat format) noexcept {
    switch (format) {
    case core::FileFormat::Mp3:
        return "mp3";
    case core::FileFormat::Wav:
        return "wav";
    case core::FileFormat::Flac:
        return "flac";
    case core::FileFormat::Aac:
        return "adts";
    case core::FileFormat::M4a:
        return "ipod";
    case core::FileFormat::Ogg:
    case core::FileFormat::Opus:
        return "ogg";
    case core::FileFormat::Mp4:
        return "mp4";
    case core::FileFormat::Mkv:
        return "matroska";
    case core::FileFormat::WebM:
        return "webm";
    default:
        return nullptr;
    }
}

} // namespace nativeshift::media::detail
