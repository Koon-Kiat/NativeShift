#include "media_operations.hpp"

#include "ffmpeg_support.hpp"

extern "C" {
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stop_token>
#include <string>

namespace nativeshift::media {
namespace {

using core::AudioCodec;
using core::ErrorCategory;
using core::FileFormat;
using core::ProviderOutcome;
using detail::AudioFifoPtr;
using detail::CodecContextPtr;
using detail::FramePtr;
using detail::InputContext;
using detail::OutputContext;
using detail::PacketPtr;
using detail::SwrPtr;

struct InterruptState {
    std::stop_token cancellation;
};

int InterruptCallback(void* opaque) {
    const auto* state = static_cast<const InterruptState*>(opaque);
    return state != nullptr && state->cancellation.stop_requested() ? 1 : 0;
}

ProviderOutcome FfmpegFailure(const ErrorCategory category,
                              const std::string_view operation,
                              const int error) {
    return ProviderOutcome::Failed(category, std::string(operation) + ": " +
                                                 detail::ErrorText(error));
}

const AVCodec* FindEncoder(const AudioCodec selected, const FileFormat output) {
    const char* preferred_name = nullptr;
    AVCodecID codec_id = AV_CODEC_ID_NONE;
    if (selected != AudioCodec::Automatic) {
        switch (selected) {
        case AudioCodec::Mp3:
            preferred_name = "libmp3lame";
            codec_id = AV_CODEC_ID_MP3;
            break;
        case AudioCodec::PcmS16:
            codec_id = AV_CODEC_ID_PCM_S16LE;
            break;
        case AudioCodec::Flac:
            codec_id = AV_CODEC_ID_FLAC;
            break;
        case AudioCodec::Aac:
            codec_id = AV_CODEC_ID_AAC;
            break;
        case AudioCodec::Vorbis:
            preferred_name = "libvorbis";
            codec_id = AV_CODEC_ID_VORBIS;
            break;
        case AudioCodec::Opus:
            preferred_name = "libopus";
            codec_id = AV_CODEC_ID_OPUS;
            break;
        case AudioCodec::Copy:
        case AudioCodec::Automatic:
            break;
        }
    } else {
        switch (output) {
        case FileFormat::Mp3:
            preferred_name = "libmp3lame";
            codec_id = AV_CODEC_ID_MP3;
            break;
        case FileFormat::Wav:
            codec_id = AV_CODEC_ID_PCM_S16LE;
            break;
        case FileFormat::Flac:
            codec_id = AV_CODEC_ID_FLAC;
            break;
        case FileFormat::Aac:
        case FileFormat::M4a:
            codec_id = AV_CODEC_ID_AAC;
            break;
        case FileFormat::Ogg:
            preferred_name = "libvorbis";
            codec_id = AV_CODEC_ID_VORBIS;
            break;
        case FileFormat::Opus:
            preferred_name = "libopus";
            codec_id = AV_CODEC_ID_OPUS;
            break;
        default:
            break;
        }
    }

    if (preferred_name != nullptr) {
        if (const auto* codec = avcodec_find_encoder_by_name(preferred_name);
            codec != nullptr) {
            return codec;
        }
    }
    return codec_id == AV_CODEC_ID_NONE ? nullptr
                                        : avcodec_find_encoder(codec_id);
}

int ChooseSampleRate(const AVCodec* encoder, const int requested) {
    const void* values = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, encoder,
                                     AV_CODEC_CONFIG_SAMPLE_RATE, 0, &values,
                                     &count) < 0 ||
        values == nullptr || count <= 0) {
        return requested;
    }
    const auto* rates = static_cast<const int*>(values);
    int chosen = rates[0];
    auto distance = std::abs(static_cast<long long>(chosen) - requested);
    for (int index = 1; index < count; ++index) {
        const auto candidate_distance =
            std::abs(static_cast<long long>(rates[index]) - requested);
        if (candidate_distance < distance) {
            chosen = rates[index];
            distance = candidate_distance;
        }
    }
    return chosen;
}

AVSampleFormat RequestedSampleFormat(const std::string& name) {
    if (name.empty()) {
        return AV_SAMPLE_FMT_NONE;
    }
    return av_get_sample_fmt(name.c_str());
}

AVSampleFormat ChooseSampleFormat(const AVCodec* encoder,
                                  const AVSampleFormat requested) {
    const void* values = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, encoder,
                                     AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &values,
                                     &count) < 0 ||
        values == nullptr || count <= 0) {
        return requested == AV_SAMPLE_FMT_NONE ? AV_SAMPLE_FMT_FLTP : requested;
    }
    const auto* formats = static_cast<const AVSampleFormat*>(values);
    if (requested != AV_SAMPLE_FMT_NONE) {
        for (int index = 0; index < count; ++index) {
            if (formats[index] == requested) {
                return requested;
            }
        }
    }
    return formats[0];
}

bool ChooseChannelLayout(const AVCodec* encoder, const int requested_channels,
                         AVChannelLayout& selected) {
    const void* values = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, encoder,
                                     AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0, &values,
                                     &count) >= 0 &&
        values != nullptr && count > 0) {
        const auto* layouts = static_cast<const AVChannelLayout*>(values);
        int best = 0;
        for (int index = 0; index < count; ++index) {
            if (layouts[index].nb_channels == requested_channels) {
                best = index;
                break;
            }
            if (layouts[index].nb_channels == 2) {
                best = index;
            }
        }
        return av_channel_layout_copy(&selected, &layouts[best]) >= 0;
    }
    av_channel_layout_default(&selected, requested_channels);
    return selected.nb_channels > 0;
}

double FractionForTimestamp(const AVFormatContext* input,
                            const AVStream* stream, const std::int64_t pts) {
    if (pts == AV_NOPTS_VALUE) {
        return 0.0;
    }
    const auto timestamp =
        av_rescale_q(pts, stream->time_base, AVRational{1, AV_TIME_BASE});
    std::int64_t duration = input->duration;
    if (duration <= 0 && stream->duration > 0) {
        duration = av_rescale_q(stream->duration, stream->time_base,
                                AVRational{1, AV_TIME_BASE});
    }
    if (duration <= 0) {
        return 0.0;
    }
    return std::clamp(static_cast<double>(timestamp) /
                          static_cast<double>(duration),
                      0.0, 1.0);
}

ProviderOutcome CopyAudio(const core::ConversionRequest& request,
                          const core::ProgressCallback& progress,
                          const std::stop_token cancellation) {
    InterruptState interrupt{cancellation};
    InputContext input;
    input.value = avformat_alloc_context();
    if (input.value == nullptr) {
        return ProviderOutcome::Failed(
            ErrorCategory::ResourceLimit,
            "FFmpeg could not allocate input state.");
    }
    input.value->interrupt_callback = {InterruptCallback, &interrupt};
    input.value->max_streams = 64;
    const auto input_name = detail::PathToUtf8(request.input_path);
    int status =
        avformat_open_input(&input.value, input_name.c_str(), nullptr, nullptr);
    if (status >= 0) {
        status = avformat_find_stream_info(input.value, nullptr);
    }
    if (status < 0) {
        return cancellation.stop_requested()
                   ? ProviderOutcome::Cancelled("Audio conversion cancelled.")
                   : FfmpegFailure(ErrorCategory::InvalidInput,
                                   "FFmpeg could not inspect the audio input",
                                   status);
    }
    int audio_index = request.audio.stream_index.value_or(av_find_best_stream(
        input.value, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0));
    if (audio_index < 0 ||
        static_cast<unsigned int>(audio_index) >= input.value->nb_streams ||
        input.value->streams[audio_index]->codecpar->codec_type !=
            AVMEDIA_TYPE_AUDIO) {
        return ProviderOutcome::Failed(
            ErrorCategory::InvalidRequest,
            "The selected audio stream does not exist or is not audio.");
    }
    AVStream* source = input.value->streams[audio_index];

    OutputContext output;
    const auto output_name = detail::PathToUtf8(request.output_path);
    status = avformat_alloc_output_context2(
        &output.value, nullptr, detail::MuxerName(request.output_format),
        output_name.c_str());
    if (status < 0 || output.value == nullptr) {
        return FfmpegFailure(ErrorCategory::Codec,
                             "FFmpeg could not create the audio container",
                             status);
    }
    output.value->interrupt_callback = {InterruptCallback, &interrupt};
    if (avformat_query_codec(output.value->oformat, source->codecpar->codec_id,
                             FF_COMPLIANCE_NORMAL) <= 0) {
        return ProviderOutcome::Failed(
            ErrorCategory::UnsupportedFormat,
            "The source audio codec cannot be copied into the requested "
            "container.");
    }
    AVStream* destination = avformat_new_stream(output.value, nullptr);
    if (destination == nullptr) {
        return ProviderOutcome::Failed(
            ErrorCategory::ResourceLimit,
            "FFmpeg could not allocate the copied audio stream.");
    }
    status = avcodec_parameters_copy(destination->codecpar, source->codecpar);
    if (status < 0) {
        return FfmpegFailure(ErrorCategory::Codec,
                             "FFmpeg could not copy audio parameters", status);
    }
    destination->codecpar->codec_tag = 0;
    destination->time_base = source->time_base;
    if (request.audio.preserve_metadata) {
        status =
            av_dict_copy(&output.value->metadata, input.value->metadata, 0);
        if (status >= 0) {
            status = av_dict_copy(&destination->metadata, source->metadata, 0);
        }
        if (status < 0) {
            return FfmpegFailure(ErrorCategory::Codec,
                                 "FFmpeg could not copy audio metadata",
                                 status);
        }
    }
    if ((output.value->oformat->flags & AVFMT_NOFILE) == 0) {
        status =
            avio_open(&output.value->pb, output_name.c_str(), AVIO_FLAG_WRITE);
        if (status < 0) {
            return FfmpegFailure(ErrorCategory::Io,
                                 "FFmpeg could not open the temporary output",
                                 status);
        }
    }
    status = avformat_write_header(output.value, nullptr);
    if (status < 0) {
        return FfmpegFailure(ErrorCategory::Codec,
                             "FFmpeg could not write the audio header", status);
    }
    PacketPtr packet(av_packet_alloc());
    if (!packet) {
        return ProviderOutcome::Failed(
            ErrorCategory::ResourceLimit,
            "FFmpeg could not allocate the bounded audio packet.");
    }
    while (!cancellation.stop_requested() &&
           (status = av_read_frame(input.value, packet.get())) >= 0) {
        if (packet->stream_index != audio_index) {
            av_packet_unref(packet.get());
            continue;
        }
        const std::int64_t timestamp =
            packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        av_packet_rescale_ts(packet.get(), source->time_base,
                             destination->time_base);
        packet->stream_index = destination->index;
        packet->pos = -1;
        status = av_interleaved_write_frame(output.value, packet.get());
        av_packet_unref(packet.get());
        if (status < 0) {
            return FfmpegFailure(ErrorCategory::Io,
                                 "FFmpeg could not copy an audio packet",
                                 status);
        }
        if (progress) {
            const double fraction =
                FractionForTimestamp(input.value, source, timestamp);
            if (fraction > 0.0) {
                progress({0.05 + fraction * 0.90, "Copying audio stream"});
            }
        }
    }
    if (cancellation.stop_requested()) {
        return ProviderOutcome::Cancelled("Audio conversion cancelled.");
    }
    if (status != AVERROR_EOF) {
        return FfmpegFailure(ErrorCategory::InvalidInput,
                             "FFmpeg could not read the complete audio input",
                             status);
    }
    status = av_write_trailer(output.value);
    if (status < 0) {
        return FfmpegFailure(ErrorCategory::Io,
                             "FFmpeg could not finish the audio container",
                             status);
    }
    auto outcome = ProviderOutcome::Succeeded();
    outcome.selected_codec = "stream-copy";
    return outcome;
}

ProviderOutcome Transcode(const core::ConversionRequest& request,
                          const core::ProgressCallback& progress,
                          const std::stop_token cancellation) {
    InterruptState interrupt{cancellation};
    InputContext input;
    input.value = avformat_alloc_context();
    if (input.value == nullptr) {
        return ProviderOutcome::Failed(
            ErrorCategory::ResourceLimit,
            "FFmpeg could not allocate input state.");
    }
    input.value->interrupt_callback = {InterruptCallback, &interrupt};
    input.value->max_streams = 64;
    input.value->probesize = 32LL * 1024LL * 1024LL;
    input.value->max_analyze_duration = 10LL * AV_TIME_BASE;

    auto input_name = detail::PathToUtf8(request.input_path);
    int status =
        avformat_open_input(&input.value, input_name.c_str(), nullptr, nullptr);
    if (status < 0) {
        return cancellation.stop_requested()
                   ? ProviderOutcome::Cancelled("Audio conversion cancelled.")
                   : FfmpegFailure(ErrorCategory::InvalidInput,
                                   "FFmpeg could not open the input", status);
    }
    status = avformat_find_stream_info(input.value, nullptr);
    if (status < 0) {
        return cancellation.stop_requested()
                   ? ProviderOutcome::Cancelled("Audio conversion cancelled.")
                   : FfmpegFailure(ErrorCategory::InvalidInput,
                                   "FFmpeg could not inspect the input",
                                   status);
    }

    int audio_index = -1;
    if (request.audio.stream_index) {
        audio_index = *request.audio.stream_index;
        if (audio_index < 0 ||
            static_cast<unsigned int>(audio_index) >= input.value->nb_streams ||
            input.value->streams[audio_index]->codecpar->codec_type !=
                AVMEDIA_TYPE_AUDIO) {
            return ProviderOutcome::Failed(
                ErrorCategory::InvalidRequest,
                "The selected audio stream does not exist or is not audio.");
        }
    } else {
        audio_index = av_find_best_stream(input.value, AVMEDIA_TYPE_AUDIO, -1,
                                          -1, nullptr, 0);
        if (audio_index < 0) {
            return ProviderOutcome::Failed(
                ErrorCategory::InvalidInput,
                "The input does not contain a decodable audio stream.");
        }
    }
    AVStream* input_stream = input.value->streams[audio_index];

    const auto* decoder =
        avcodec_find_decoder(input_stream->codecpar->codec_id);
    if (decoder == nullptr) {
        return ProviderOutcome::Failed(
            ErrorCategory::Codec,
            "No decoder is available for the selected audio stream.");
    }
    CodecContextPtr decoder_context(avcodec_alloc_context3(decoder));
    if (!decoder_context) {
        return ProviderOutcome::Failed(ErrorCategory::ResourceLimit,
                                       "FFmpeg could not allocate a decoder.");
    }
    status = avcodec_parameters_to_context(decoder_context.get(),
                                           input_stream->codecpar);
    if (status < 0) {
        return FfmpegFailure(ErrorCategory::Codec,
                             "FFmpeg could not configure the decoder", status);
    }
    decoder_context->pkt_timebase = input_stream->time_base;
    status = avcodec_open2(decoder_context.get(), decoder, nullptr);
    if (status < 0) {
        return FfmpegFailure(ErrorCategory::Codec,
                             "FFmpeg could not open the decoder", status);
    }

    const auto* encoder =
        FindEncoder(request.audio.codec, request.output_format);
    if (encoder == nullptr) {
        return ProviderOutcome::Failed(
            ErrorCategory::Codec,
            "The requested audio encoder is unavailable in this FFmpeg build.");
    }

    OutputContext output;
    const auto output_name = detail::PathToUtf8(request.output_path);
    status = avformat_alloc_output_context2(
        &output.value, nullptr, detail::MuxerName(request.output_format),
        output_name.c_str());
    if (status < 0 || output.value == nullptr) {
        return FfmpegFailure(ErrorCategory::Codec,
                             "FFmpeg could not create the output container",
                             status);
    }
    output.value->interrupt_callback = {InterruptCallback, &interrupt};

    AVStream* output_stream = avformat_new_stream(output.value, nullptr);
    if (output_stream == nullptr) {
        return ProviderOutcome::Failed(
            ErrorCategory::ResourceLimit,
            "FFmpeg could not allocate the output audio stream.");
    }
    CodecContextPtr encoder_context(avcodec_alloc_context3(encoder));
    if (!encoder_context) {
        return ProviderOutcome::Failed(ErrorCategory::ResourceLimit,
                                       "FFmpeg could not allocate an encoder.");
    }

    const int requested_rate = request.audio.sample_rate.value_or(
        decoder_context->sample_rate > 0 ? decoder_context->sample_rate
                                         : 48'000);
    encoder_context->sample_rate = ChooseSampleRate(encoder, requested_rate);
    encoder_context->sample_fmt = ChooseSampleFormat(
        encoder, RequestedSampleFormat(request.audio.sample_format));
    int requested_channels =
        request.audio.channels.value_or(decoder_context->ch_layout.nb_channels);
    if (requested_channels <= 0) {
        requested_channels = 2;
    }
    if (!ChooseChannelLayout(encoder, requested_channels,
                             encoder_context->ch_layout)) {
        return ProviderOutcome::Failed(
            ErrorCategory::Codec,
            "The requested channel layout is unavailable for the encoder.");
    }
    encoder_context->time_base = {1, encoder_context->sample_rate};
    encoder_context->bit_rate =
        static_cast<std::int64_t>(request.audio.bitrate_kbps) * 1'000LL;
    if (request.audio.variable_bitrate) {
        encoder_context->flags |= AV_CODEC_FLAG_QSCALE;
        encoder_context->global_quality = 4 * FF_QP2LAMBDA;
    }
    if ((output.value->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        encoder_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    status = avcodec_open2(encoder_context.get(), encoder, nullptr);
    if (status < 0) {
        return FfmpegFailure(
            ErrorCategory::Codec,
            "FFmpeg could not open the requested audio encoder", status);
    }
    output_stream->time_base = encoder_context->time_base;
    status = avcodec_parameters_from_context(output_stream->codecpar,
                                             encoder_context.get());
    if (status < 0) {
        return FfmpegFailure(
            ErrorCategory::Codec,
            "FFmpeg could not configure the output audio stream", status);
    }

    if (request.audio.preserve_metadata) {
        status =
            av_dict_copy(&output.value->metadata, input.value->metadata, 0);
        if (status < 0) {
            return FfmpegFailure(ErrorCategory::Codec,
                                 "FFmpeg could not copy metadata", status);
        }
        status =
            av_dict_copy(&output_stream->metadata, input_stream->metadata, 0);
        if (status < 0) {
            return FfmpegFailure(ErrorCategory::Codec,
                                 "FFmpeg could not copy stream metadata",
                                 status);
        }
    }

    if ((output.value->oformat->flags & AVFMT_NOFILE) == 0) {
        status =
            avio_open(&output.value->pb, output_name.c_str(), AVIO_FLAG_WRITE);
        if (status < 0) {
            return FfmpegFailure(ErrorCategory::Io,
                                 "FFmpeg could not open the temporary output",
                                 status);
        }
    }
    status = avformat_write_header(output.value, nullptr);
    if (status < 0) {
        return FfmpegFailure(ErrorCategory::Codec,
                             "FFmpeg could not write the output header",
                             status);
    }

    SwrContext* raw_resampler = nullptr;
    status = swr_alloc_set_opts2(
        &raw_resampler, &encoder_context->ch_layout,
        encoder_context->sample_fmt, encoder_context->sample_rate,
        &decoder_context->ch_layout, decoder_context->sample_fmt,
        decoder_context->sample_rate, 0, nullptr);
    SwrPtr resampler(raw_resampler);
    if (status < 0 || !resampler) {
        return FfmpegFailure(ErrorCategory::Codec,
                             "FFmpeg could not configure audio resampling",
                             status);
    }
    status = swr_init(resampler.get());
    if (status < 0) {
        return FfmpegFailure(ErrorCategory::Codec,
                             "FFmpeg could not initialize audio resampling",
                             status);
    }

    const int encoder_frame_size =
        encoder_context->frame_size > 0 ? encoder_context->frame_size : 1'024;
    AudioFifoPtr fifo(av_audio_fifo_alloc(
        encoder_context->sample_fmt, encoder_context->ch_layout.nb_channels,
        encoder_frame_size * 2));
    PacketPtr input_packet(av_packet_alloc());
    PacketPtr output_packet(av_packet_alloc());
    FramePtr decoded_frame(av_frame_alloc());
    if (!fifo || !input_packet || !output_packet || !decoded_frame) {
        return ProviderOutcome::Failed(
            ErrorCategory::ResourceLimit,
            "FFmpeg could not allocate bounded audio buffers.");
    }

    std::int64_t next_pts = 0;
    auto encode_available =
        [&](const bool final) -> std::optional<ProviderOutcome> {
        const int frame_size = encoder_context->frame_size;
        while (av_audio_fifo_size(fifo.get()) >=
                   (frame_size > 0 ? frame_size : 1) ||
               (final && av_audio_fifo_size(fifo.get()) > 0)) {
            const int available = av_audio_fifo_size(fifo.get());
            const int samples =
                frame_size > 0 ? std::min(frame_size, available) : available;
            FramePtr frame(av_frame_alloc());
            if (!frame) {
                return ProviderOutcome::Failed(
                    ErrorCategory::ResourceLimit,
                    "FFmpeg could not allocate an encoder frame.");
            }
            frame->nb_samples = samples;
            frame->format = encoder_context->sample_fmt;
            frame->sample_rate = encoder_context->sample_rate;
            if (av_channel_layout_copy(&frame->ch_layout,
                                       &encoder_context->ch_layout) < 0) {
                return ProviderOutcome::Failed(
                    ErrorCategory::ResourceLimit,
                    "FFmpeg could not copy the encoder channel layout.");
            }
            status = av_frame_get_buffer(frame.get(), 0);
            if (status < 0) {
                return FfmpegFailure(
                    ErrorCategory::ResourceLimit,
                    "FFmpeg could not allocate encoder sample data", status);
            }
            if (av_audio_fifo_read(
                    fifo.get(), reinterpret_cast<void**>(frame->extended_data),
                    samples) != samples) {
                return ProviderOutcome::Failed(
                    ErrorCategory::Codec,
                    "FFmpeg could not read the resampled audio buffer.");
            }
            frame->pts = next_pts;
            next_pts += samples;
            status = avcodec_send_frame(encoder_context.get(), frame.get());
            if (status < 0) {
                return FfmpegFailure(ErrorCategory::Codec,
                                     "The audio encoder rejected a frame",
                                     status);
            }
            while (true) {
                status = avcodec_receive_packet(encoder_context.get(),
                                                output_packet.get());
                if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                    break;
                }
                if (status < 0) {
                    return FfmpegFailure(
                        ErrorCategory::Codec,
                        "The audio encoder failed to produce a packet", status);
                }
                av_packet_rescale_ts(output_packet.get(),
                                     encoder_context->time_base,
                                     output_stream->time_base);
                output_packet->stream_index = output_stream->index;
                status = av_interleaved_write_frame(output.value,
                                                    output_packet.get());
                av_packet_unref(output_packet.get());
                if (status < 0) {
                    return FfmpegFailure(ErrorCategory::Io,
                                         "FFmpeg could not write encoded audio",
                                         status);
                }
            }
        }
        return std::nullopt;
    };

    auto resample_frame =
        [&](const AVFrame* frame) -> std::optional<ProviderOutcome> {
        const std::int64_t delayed =
            swr_get_delay(resampler.get(), decoder_context->sample_rate);
        const auto capacity64 = av_rescale_rnd(
            delayed + frame->nb_samples, encoder_context->sample_rate,
            decoder_context->sample_rate, AV_ROUND_UP);
        if (capacity64 <= 0 || capacity64 > std::numeric_limits<int>::max()) {
            return ProviderOutcome::Failed(
                ErrorCategory::ResourceLimit,
                "The resampled audio buffer would exceed the safe limit.");
        }
        const int capacity = static_cast<int>(capacity64);
        FramePtr converted(av_frame_alloc());
        if (!converted) {
            return ProviderOutcome::Failed(
                ErrorCategory::ResourceLimit,
                "FFmpeg could not allocate a resampling frame.");
        }
        converted->nb_samples = capacity;
        converted->format = encoder_context->sample_fmt;
        converted->sample_rate = encoder_context->sample_rate;
        if (av_channel_layout_copy(&converted->ch_layout,
                                   &encoder_context->ch_layout) < 0) {
            return ProviderOutcome::Failed(
                ErrorCategory::ResourceLimit,
                "FFmpeg could not copy the resampling channel layout.");
        }
        status = av_frame_get_buffer(converted.get(), 0);
        if (status < 0) {
            return FfmpegFailure(
                ErrorCategory::ResourceLimit,
                "FFmpeg could not allocate resampled audio data", status);
        }
        status =
            swr_convert(resampler.get(), converted->extended_data, capacity,
                        const_cast<const std::uint8_t**>(frame->extended_data),
                        frame->nb_samples);
        if (status < 0) {
            return FfmpegFailure(ErrorCategory::Codec,
                                 "FFmpeg could not resample audio", status);
        }
        if (status == 0) {
            return std::nullopt;
        }
        if (av_audio_fifo_realloc(fifo.get(), av_audio_fifo_size(fifo.get()) +
                                                  status) < 0) {
            return ProviderOutcome::Failed(
                ErrorCategory::ResourceLimit,
                "The bounded audio queue could not be enlarged.");
        }
        if (av_audio_fifo_write(
                fifo.get(), reinterpret_cast<void**>(converted->extended_data),
                status) != status) {
            return ProviderOutcome::Failed(
                ErrorCategory::Codec,
                "FFmpeg could not queue resampled audio.");
        }
        return encode_available(false);
    };

    auto drain_decoder = [&]() -> std::optional<ProviderOutcome> {
        while (true) {
            status = avcodec_receive_frame(decoder_context.get(),
                                           decoded_frame.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return std::nullopt;
            }
            if (status < 0) {
                return FfmpegFailure(
                    ErrorCategory::InvalidInput,
                    "The audio decoder rejected the input stream", status);
            }
            if (auto failure = resample_frame(decoded_frame.get())) {
                return failure;
            }
            const auto fraction =
                FractionForTimestamp(input.value, input_stream,
                                     decoded_frame->best_effort_timestamp);
            if (progress && fraction > 0.0) {
                progress({0.05 + fraction * 0.90, "Transcoding audio"});
            }
            av_frame_unref(decoded_frame.get());
        }
    };

    while (!cancellation.stop_requested() &&
           (status = av_read_frame(input.value, input_packet.get())) >= 0) {
        if (input_packet->stream_index == audio_index) {
            status =
                avcodec_send_packet(decoder_context.get(), input_packet.get());
            av_packet_unref(input_packet.get());
            if (status < 0) {
                return FfmpegFailure(
                    ErrorCategory::InvalidInput,
                    "The audio decoder rejected an input packet", status);
            }
            if (auto failure = drain_decoder()) {
                return *failure;
            }
        } else {
            av_packet_unref(input_packet.get());
        }
    }
    if (cancellation.stop_requested()) {
        return ProviderOutcome::Cancelled("Audio conversion cancelled.");
    }
    if (status != AVERROR_EOF) {
        return FfmpegFailure(ErrorCategory::InvalidInput,
                             "FFmpeg could not read the complete input",
                             status);
    }

    status = avcodec_send_packet(decoder_context.get(), nullptr);
    if (status < 0) {
        return FfmpegFailure(ErrorCategory::Codec,
                             "FFmpeg could not flush the audio decoder",
                             status);
    }
    if (auto failure = drain_decoder()) {
        return *failure;
    }

    while (swr_get_delay(resampler.get(), decoder_context->sample_rate) > 0) {
        const auto capacity64 = av_rescale_rnd(
            swr_get_delay(resampler.get(), decoder_context->sample_rate),
            encoder_context->sample_rate, decoder_context->sample_rate,
            AV_ROUND_UP);
        if (capacity64 <= 0 || capacity64 > std::numeric_limits<int>::max()) {
            break;
        }
        FramePtr converted(av_frame_alloc());
        if (!converted) {
            return ProviderOutcome::Failed(
                ErrorCategory::ResourceLimit,
                "FFmpeg could not allocate the final resampling frame.");
        }
        converted->nb_samples = static_cast<int>(capacity64);
        converted->format = encoder_context->sample_fmt;
        converted->sample_rate = encoder_context->sample_rate;
        if (av_channel_layout_copy(&converted->ch_layout,
                                   &encoder_context->ch_layout) < 0 ||
            av_frame_get_buffer(converted.get(), 0) < 0) {
            return ProviderOutcome::Failed(
                ErrorCategory::ResourceLimit,
                "FFmpeg could not allocate final resampled audio data.");
        }
        const int converted_samples =
            swr_convert(resampler.get(), converted->extended_data,
                        converted->nb_samples, nullptr, 0);
        if (converted_samples <= 0) {
            break;
        }
        if (av_audio_fifo_realloc(fifo.get(), av_audio_fifo_size(fifo.get()) +
                                                  converted_samples) < 0 ||
            av_audio_fifo_write(
                fifo.get(), reinterpret_cast<void**>(converted->extended_data),
                converted_samples) != converted_samples) {
            return ProviderOutcome::Failed(
                ErrorCategory::ResourceLimit,
                "FFmpeg could not queue the final resampled audio.");
        }
    }
    if (auto failure = encode_available(true)) {
        return *failure;
    }
    status = avcodec_send_frame(encoder_context.get(), nullptr);
    if (status < 0) {
        return FfmpegFailure(ErrorCategory::Codec,
                             "FFmpeg could not flush the audio encoder",
                             status);
    }
    while (true) {
        status =
            avcodec_receive_packet(encoder_context.get(), output_packet.get());
        if (status == AVERROR_EOF) {
            break;
        }
        if (status == AVERROR(EAGAIN)) {
            continue;
        }
        if (status < 0) {
            return FfmpegFailure(ErrorCategory::Codec,
                                 "The audio encoder failed while flushing",
                                 status);
        }
        av_packet_rescale_ts(output_packet.get(), encoder_context->time_base,
                             output_stream->time_base);
        output_packet->stream_index = output_stream->index;
        status = av_interleaved_write_frame(output.value, output_packet.get());
        av_packet_unref(output_packet.get());
        if (status < 0) {
            return FfmpegFailure(
                ErrorCategory::Io,
                "FFmpeg could not write the final audio packet", status);
        }
    }
    status = av_write_trailer(output.value);
    if (status < 0) {
        return FfmpegFailure(ErrorCategory::Io,
                             "FFmpeg could not finish the audio container",
                             status);
    }

    auto outcome = ProviderOutcome::Succeeded();
    outcome.selected_codec = encoder->name;
    if (request.audio.preserve_source_settings &&
        (encoder_context->sample_rate != decoder_context->sample_rate ||
         encoder_context->ch_layout.nb_channels !=
             decoder_context->ch_layout.nb_channels)) {
        outcome.warnings.push_back(
            "The encoder did not support every source audio setting; "
            "NativeShift used the nearest compatible configuration.");
    }
    return outcome;
}

} // namespace

ProviderOutcome TranscodeAudio(const core::ConversionRequest& request,
                               const core::ProgressCallback& progress,
                               const std::stop_token cancellation) {
    if (request.audio.codec == AudioCodec::Copy) {
        return CopyAudio(request, progress, cancellation);
    }
    return Transcode(request, progress, cancellation);
}

} // namespace nativeshift::media
