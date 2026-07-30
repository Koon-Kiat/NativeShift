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
#include <string_view>
#include <utility>
#include <vector>

namespace nativeshift::media {
namespace {

using core::ErrorCategory;
using core::HardwareAcceleration;
using core::ProviderOutcome;
using core::VideoAudioCodec;
using core::VideoCodec;
using detail::AudioFifoPtr;
using detail::CodecContextPtr;
using detail::FramePtr;
using detail::InputContext;
using detail::OutputContext;
using detail::PacketPtr;
using detail::SwrPtr;
using detail::SwsPtr;

struct InterruptState {
    std::stop_token cancellation;
};

struct EncoderSelection {
    const AVCodec* codec{};
    CodecContextPtr context;
    std::string name;
    std::vector<std::string> warnings;
};

int InterruptCallback(void* opaque) {
    const auto* state = static_cast<const InterruptState*>(opaque);
    return state != nullptr && state->cancellation.stop_requested() ? 1 : 0;
}

ProviderOutcome Failure(const ErrorCategory category,
                        const std::string_view operation, const int error) {
    return ProviderOutcome::Failed(category, std::string(operation) + ": " +
                                                 detail::ErrorText(error));
}

bool IsHardwareEncoderName(const std::string_view name) {
    return name.ends_with("_nvenc") || name.ends_with("_qsv") ||
           name.ends_with("_amf") || name.ends_with("_mf");
}

std::vector<std::string> EncoderCandidates(const core::VideoOptions& options,
                                           const core::FileFormat output) {
    if (options.hardware_acceleration == HardwareAcceleration::Specific) {
        return {options.specific_encoder};
    }

    VideoCodec codec = options.video_codec;
    if (codec == VideoCodec::Automatic) {
        codec = output == core::FileFormat::WebM ? VideoCodec::Vp9
                                                 : VideoCodec::H264;
    }
    std::vector<std::string> hardware;
    std::vector<std::string> software;
    switch (codec) {
    case VideoCodec::H264:
        hardware = {"h264_nvenc", "h264_qsv", "h264_amf", "h264_mf"};
        software = {"libopenh264"};
        break;
    case VideoCodec::H265:
        hardware = {"hevc_nvenc", "hevc_qsv", "hevc_amf", "hevc_mf"};
        break;
    case VideoCodec::Vp9:
        hardware = {"vp9_qsv"};
        software = {"libvpx-vp9"};
        break;
    case VideoCodec::Av1:
        hardware = {"av1_nvenc", "av1_qsv", "av1_amf"};
        software = {"libaom-av1"};
        break;
    case VideoCodec::Automatic:
    case VideoCodec::Copy:
        break;
    }

    if (options.hardware_acceleration == HardwareAcceleration::SoftwareOnly) {
        return software;
    }
    hardware.insert(hardware.end(), software.begin(), software.end());
    return hardware;
}

AVPixelFormat ChoosePixelFormat(const AVCodec* encoder) {
    const void* values = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, encoder,
                                     AV_CODEC_CONFIG_PIX_FORMAT, 0, &values,
                                     &count) < 0 ||
        values == nullptr || count <= 0) {
        return AV_PIX_FMT_YUV420P;
    }
    const auto* formats = static_cast<const AVPixelFormat*>(values);
    for (int index = 0; index < count; ++index) {
        if (formats[index] == AV_PIX_FMT_YUV420P) {
            return AV_PIX_FMT_YUV420P;
        }
    }
    return formats[0];
}

std::pair<int, int> OutputDimensions(const core::VideoOptions& options,
                                     const int source_width,
                                     const int source_height) {
    int width = options.width ? static_cast<int>(*options.width) : source_width;
    int height =
        options.height ? static_cast<int>(*options.height) : source_height;
    if (options.preserve_aspect_ratio && source_width > 0 &&
        source_height > 0) {
        if (options.width && options.height) {
            const double scale = std::min(
                static_cast<double>(width) / static_cast<double>(source_width),
                static_cast<double>(height) /
                    static_cast<double>(source_height));
            width = static_cast<int>(
                std::round(static_cast<double>(source_width) * scale));
            height = static_cast<int>(
                std::round(static_cast<double>(source_height) * scale));
        } else if (options.width) {
            height =
                static_cast<int>(std::round(static_cast<double>(source_height) *
                                            static_cast<double>(width) /
                                            static_cast<double>(source_width)));
        } else if (options.height) {
            width = static_cast<int>(
                std::round(static_cast<double>(source_width) *
                           static_cast<double>(height) /
                           static_cast<double>(source_height)));
        }
    }
    width = std::max(2, width - (width % 2));
    height = std::max(2, height - (height % 2));
    return {width, height};
}

AVRational OutputFrameRate(const core::VideoOptions& options,
                           AVFormatContext* input, AVStream* stream) {
    if (options.frame_rate) {
        return av_d2q(*options.frame_rate, 1'000'000);
    }
    AVRational rate = av_guess_frame_rate(input, stream, nullptr);
    if (rate.num <= 0 || rate.den <= 0) {
        rate = {30, 1};
    }
    return rate;
}

EncoderSelection OpenEncoder(const core::ConversionRequest& request,
                             AVFormatContext* input, AVStream* source_stream,
                             const AVCodecContext* decoder,
                             const AVOutputFormat* output_format) {
    EncoderSelection selection;
    const auto candidates =
        EncoderCandidates(request.video, request.output_format);
    int last_error = AVERROR_ENCODER_NOT_FOUND;
    bool attempted_hardware = false;
    const auto [width, height] =
        OutputDimensions(request.video, decoder->width, decoder->height);
    const AVRational frame_rate =
        OutputFrameRate(request.video, input, source_stream);

    for (const auto& name : candidates) {
        const auto* encoder = avcodec_find_encoder_by_name(name.c_str());
        if (encoder == nullptr ||
            avformat_query_codec(output_format, encoder->id,
                                 FF_COMPLIANCE_NORMAL) <= 0) {
            continue;
        }
        const bool hardware = IsHardwareEncoderName(name);
        attempted_hardware |= hardware;
        CodecContextPtr context(avcodec_alloc_context3(encoder));
        if (!context) {
            last_error = AVERROR(ENOMEM);
            continue;
        }
        context->width = width;
        context->height = height;
        context->pix_fmt = ChoosePixelFormat(encoder);
        context->framerate = frame_rate;
        context->time_base = av_inv_q(frame_rate);
        context->sample_aspect_ratio = decoder->sample_aspect_ratio.num > 0
                                           ? decoder->sample_aspect_ratio
                                           : AVRational{1, 1};
        context->color_range = decoder->color_range;
        context->color_primaries = decoder->color_primaries;
        context->color_trc = decoder->color_trc;
        context->colorspace = decoder->colorspace;
        context->gop_size =
            std::max(1, static_cast<int>(std::round(av_q2d(frame_rate) * 2.0)));
        context->max_b_frames = encoder->id == AV_CODEC_ID_H264 ? 2 : 0;
        if (request.video.video_bitrate_kbps) {
            context->bit_rate =
                static_cast<std::int64_t>(*request.video.video_bitrate_kbps) *
                1'000LL;
        } else {
            context->bit_rate = 4'000'000;
        }
        if ((output_format->flags & AVFMT_GLOBALHEADER) != 0) {
            context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        if (request.video.quality) {
            const auto quality = std::to_string(*request.video.quality);
            if (av_opt_set(context->priv_data, "crf", quality.c_str(), 0) < 0) {
                context->flags |= AV_CODEC_FLAG_QSCALE;
                context->global_quality = *request.video.quality * FF_QP2LAMBDA;
            }
        }
        if (!request.video.encoder_preset.empty()) {
            (void)av_opt_set(context->priv_data, "preset",
                             request.video.encoder_preset.c_str(), 0);
        }

        last_error = avcodec_open2(context.get(), encoder, nullptr);
        if (last_error >= 0) {
            selection.codec = encoder;
            selection.context = std::move(context);
            selection.name = name;
            if (!hardware && attempted_hardware) {
                selection.warnings.push_back(
                    "Hardware video encoders were unavailable or failed to "
                    "initialize; NativeShift fell back to " +
                    name + ".");
            }
            return selection;
        }
        if (request.video.hardware_acceleration ==
            HardwareAcceleration::Specific) {
            break;
        }
    }
    selection.warnings.push_back(
        "No requested video encoder could be initialized: " +
        detail::ErrorText(last_error));
    return selection;
}

bool AudioCopyCompatible(const AVOutputFormat* output,
                         const AVCodecParameters* parameters,
                         const VideoAudioCodec selected) {
    if (selected == VideoAudioCodec::None) {
        return false;
    }
    if (selected != VideoAudioCodec::Automatic &&
        selected != VideoAudioCodec::Copy) {
        const bool selected_matches =
            (selected == VideoAudioCodec::Aac &&
             parameters->codec_id == AV_CODEC_ID_AAC) ||
            (selected == VideoAudioCodec::Mp3 &&
             parameters->codec_id == AV_CODEC_ID_MP3) ||
            (selected == VideoAudioCodec::Opus &&
             parameters->codec_id == AV_CODEC_ID_OPUS) ||
            (selected == VideoAudioCodec::Vorbis &&
             parameters->codec_id == AV_CODEC_ID_VORBIS);
        if (!selected_matches) {
            return false;
        }
    }
    return avformat_query_codec(output, parameters->codec_id,
                                FF_COMPLIANCE_NORMAL) > 0;
}

const AVCodec* FindAudioEncoder(const VideoAudioCodec selected,
                                const core::FileFormat output) {
    const auto effective =
        selected == VideoAudioCodec::Automatic
            ? (output == core::FileFormat::WebM ? VideoAudioCodec::Opus
                                                : VideoAudioCodec::Aac)
            : selected;
    const char* preferred_name = nullptr;
    AVCodecID codec_id = AV_CODEC_ID_NONE;
    switch (effective) {
    case VideoAudioCodec::Aac:
        codec_id = AV_CODEC_ID_AAC;
        break;
    case VideoAudioCodec::Mp3:
        preferred_name = "libmp3lame";
        codec_id = AV_CODEC_ID_MP3;
        break;
    case VideoAudioCodec::Opus:
        preferred_name = "libopus";
        codec_id = AV_CODEC_ID_OPUS;
        break;
    case VideoAudioCodec::Vorbis:
        preferred_name = "libvorbis";
        codec_id = AV_CODEC_ID_VORBIS;
        break;
    case VideoAudioCodec::Automatic:
    case VideoAudioCodec::Copy:
    case VideoAudioCodec::None:
        break;
    }
    if (preferred_name != nullptr) {
        if (const auto* encoder = avcodec_find_encoder_by_name(preferred_name);
            encoder != nullptr) {
            return encoder;
        }
    }
    return codec_id == AV_CODEC_ID_NONE ? nullptr
                                        : avcodec_find_encoder(codec_id);
}

int ChooseAudioSampleRate(const AVCodec* encoder, const int requested) {
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

AVSampleFormat ChooseAudioSampleFormat(const AVCodec* encoder) {
    const void* values = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, encoder,
                                     AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &values,
                                     &count) < 0 ||
        values == nullptr || count <= 0) {
        return AV_SAMPLE_FMT_FLTP;
    }
    return static_cast<const AVSampleFormat*>(values)[0];
}

bool ChooseAudioChannelLayout(const AVCodec* encoder,
                              const int requested_channels,
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

ProviderOutcome EncodeVideo(const core::ConversionRequest& request,
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
    input.value->probesize = 64LL * 1024LL * 1024LL;
    input.value->max_analyze_duration = 15LL * AV_TIME_BASE;

    const auto input_name = detail::PathToUtf8(request.input_path);
    int status =
        avformat_open_input(&input.value, input_name.c_str(), nullptr, nullptr);
    if (status < 0) {
        return cancellation.stop_requested()
                   ? ProviderOutcome::Cancelled("Video conversion cancelled.")
                   : Failure(ErrorCategory::InvalidInput,
                             "FFmpeg could not open the video input", status);
    }
    status = avformat_find_stream_info(input.value, nullptr);
    if (status < 0) {
        return cancellation.stop_requested()
                   ? ProviderOutcome::Cancelled("Video conversion cancelled.")
                   : Failure(ErrorCategory::InvalidInput,
                             "FFmpeg could not inspect the video input",
                             status);
    }

    int video_index =
        request.video.video_stream_index.value_or(av_find_best_stream(
            input.value, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0));
    if (video_index < 0 ||
        static_cast<unsigned int>(video_index) >= input.value->nb_streams ||
        input.value->streams[video_index]->codecpar->codec_type !=
            AVMEDIA_TYPE_VIDEO) {
        return ProviderOutcome::Failed(
            ErrorCategory::InvalidRequest,
            "The selected video stream does not exist or is not video.");
    }
    AVStream* source_video = input.value->streams[video_index];
    const auto* decoder =
        avcodec_find_decoder(source_video->codecpar->codec_id);
    if (decoder == nullptr) {
        return ProviderOutcome::Failed(
            ErrorCategory::Codec,
            "No decoder is available for the selected video stream.");
    }
    CodecContextPtr decoder_context(avcodec_alloc_context3(decoder));
    if (!decoder_context) {
        return ProviderOutcome::Failed(ErrorCategory::ResourceLimit,
                                       "FFmpeg could not allocate a decoder.");
    }
    status = avcodec_parameters_to_context(decoder_context.get(),
                                           source_video->codecpar);
    if (status < 0) {
        return Failure(ErrorCategory::Codec,
                       "FFmpeg could not configure the video decoder", status);
    }
    decoder_context->pkt_timebase = source_video->time_base;
    status = avcodec_open2(decoder_context.get(), decoder, nullptr);
    if (status < 0) {
        return Failure(ErrorCategory::Codec,
                       "FFmpeg could not open the video decoder", status);
    }

    const auto output_name = detail::PathToUtf8(request.output_path);
    OutputContext output;
    status = avformat_alloc_output_context2(
        &output.value, nullptr, detail::MuxerName(request.output_format),
        output_name.c_str());
    if (status < 0 || output.value == nullptr) {
        return Failure(ErrorCategory::Codec,
                       "FFmpeg could not create the video container", status);
    }
    output.value->interrupt_callback = {InterruptCallback, &interrupt};

    auto encoder = OpenEncoder(request, input.value, source_video,
                               decoder_context.get(), output.value->oformat);
    if (!encoder.context) {
        return ProviderOutcome::Failed(
            ErrorCategory::Codec,
            encoder.warnings.empty()
                ? "No requested video encoder is available."
                : encoder.warnings.back());
    }
    AVStream* destination_video = avformat_new_stream(output.value, nullptr);
    if (destination_video == nullptr) {
        return ProviderOutcome::Failed(
            ErrorCategory::ResourceLimit,
            "FFmpeg could not allocate the output video stream.");
    }
    destination_video->time_base = encoder.context->time_base;
    destination_video->avg_frame_rate = encoder.context->framerate;
    destination_video->sample_aspect_ratio =
        encoder.context->sample_aspect_ratio;
    status = avcodec_parameters_from_context(destination_video->codecpar,
                                             encoder.context.get());
    if (status < 0) {
        return Failure(ErrorCategory::Codec,
                       "FFmpeg could not configure the output video stream",
                       status);
    }
    status =
        av_dict_copy(&destination_video->metadata, source_video->metadata, 0);
    if (status < 0) {
        return Failure(ErrorCategory::Codec,
                       "FFmpeg could not copy video stream metadata", status);
    }

    std::vector<int> stream_map(input.value->nb_streams, -1);
    int audio_index =
        request.video.audio_stream_index.value_or(av_find_best_stream(
            input.value, AVMEDIA_TYPE_AUDIO, -1, video_index, nullptr, 0));
    if (request.video.audio_stream_index &&
        (audio_index < 0 ||
         static_cast<unsigned int>(audio_index) >= input.value->nb_streams ||
         input.value->streams[audio_index]->codecpar->codec_type !=
             AVMEDIA_TYPE_AUDIO)) {
        return ProviderOutcome::Failed(
            ErrorCategory::InvalidRequest,
            "The selected audio stream does not exist or is not audio.");
    }

    CodecContextPtr audio_decoder_context;
    CodecContextPtr audio_encoder_context;
    SwrPtr audio_resampler;
    AudioFifoPtr audio_fifo;
    AVStream* destination_audio = nullptr;
    std::string selected_audio_codec;
    bool transcode_audio = false;
    int discarded_streams = 0;
    if (audio_index >= 0 &&
        static_cast<unsigned int>(audio_index) < input.value->nb_streams &&
        request.video.audio_codec != VideoAudioCodec::None) {
        AVStream* source_audio = input.value->streams[audio_index];
        if (AudioCopyCompatible(output.value->oformat, source_audio->codecpar,
                                request.video.audio_codec)) {
            AVStream* destination = avformat_new_stream(output.value, nullptr);
            if (destination == nullptr) {
                return ProviderOutcome::Failed(
                    ErrorCategory::ResourceLimit,
                    "FFmpeg could not allocate the output audio stream.");
            }
            status = avcodec_parameters_copy(destination->codecpar,
                                             source_audio->codecpar);
            if (status < 0) {
                return Failure(ErrorCategory::Codec,
                               "FFmpeg could not copy audio parameters",
                               status);
            }
            destination->codecpar->codec_tag = 0;
            destination->time_base = source_audio->time_base;
            destination->disposition = source_audio->disposition;
            if (request.video.preserve_metadata) {
                status = av_dict_copy(&destination->metadata,
                                      source_audio->metadata, 0);
                if (status < 0) {
                    return Failure(ErrorCategory::Codec,
                                   "FFmpeg could not copy audio metadata",
                                   status);
                }
            }
            stream_map[static_cast<std::size_t>(audio_index)] =
                destination->index;
        } else {
            if (request.video.audio_codec == VideoAudioCodec::Copy) {
                return ProviderOutcome::Failed(
                    ErrorCategory::UnsupportedFormat,
                    "The selected source audio stream cannot be copied into "
                    "the requested container.");
            }
            const auto* audio_decoder =
                avcodec_find_decoder(source_audio->codecpar->codec_id);
            const auto* audio_encoder = FindAudioEncoder(
                request.video.audio_codec, request.output_format);
            if (audio_decoder == nullptr || audio_encoder == nullptr ||
                avformat_query_codec(output.value->oformat, audio_encoder->id,
                                     FF_COMPLIANCE_NORMAL) <= 0) {
                return ProviderOutcome::Failed(
                    ErrorCategory::Codec,
                    "The requested video audio codec is unavailable or "
                    "incompatible with the output container.");
            }
            audio_decoder_context.reset(avcodec_alloc_context3(audio_decoder));
            audio_encoder_context.reset(avcodec_alloc_context3(audio_encoder));
            destination_audio = avformat_new_stream(output.value, nullptr);
            if (!audio_decoder_context || !audio_encoder_context ||
                destination_audio == nullptr) {
                return ProviderOutcome::Failed(
                    ErrorCategory::ResourceLimit,
                    "FFmpeg could not allocate audio transcoding state.");
            }
            status = avcodec_parameters_to_context(audio_decoder_context.get(),
                                                   source_audio->codecpar);
            if (status < 0) {
                return Failure(ErrorCategory::Codec,
                               "FFmpeg could not configure the audio decoder",
                               status);
            }
            audio_decoder_context->pkt_timebase = source_audio->time_base;
            status = avcodec_open2(audio_decoder_context.get(), audio_decoder,
                                   nullptr);
            if (status < 0) {
                return Failure(ErrorCategory::Codec,
                               "FFmpeg could not open the audio decoder",
                               status);
            }

            const int source_rate = audio_decoder_context->sample_rate > 0
                                        ? audio_decoder_context->sample_rate
                                        : 48'000;
            audio_encoder_context->sample_rate = ChooseAudioSampleRate(
                audio_encoder,
                request.video.audio_sample_rate.value_or(source_rate));
            audio_encoder_context->sample_fmt =
                ChooseAudioSampleFormat(audio_encoder);
            int source_channels = audio_decoder_context->ch_layout.nb_channels;
            if (source_channels <= 0) {
                source_channels = 2;
            }
            if (!ChooseAudioChannelLayout(
                    audio_encoder,
                    request.video.audio_channels.value_or(source_channels),
                    audio_encoder_context->ch_layout)) {
                return ProviderOutcome::Failed(
                    ErrorCategory::Codec,
                    "The requested audio channel layout is unavailable.");
            }
            audio_encoder_context->time_base = {
                1, audio_encoder_context->sample_rate};
            audio_encoder_context->bit_rate =
                static_cast<std::int64_t>(request.video.audio_bitrate_kbps) *
                1'000LL;
            if ((output.value->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
                audio_encoder_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }
            status = avcodec_open2(audio_encoder_context.get(), audio_encoder,
                                   nullptr);
            if (status < 0) {
                return Failure(
                    ErrorCategory::Codec,
                    "FFmpeg could not open the requested audio encoder",
                    status);
            }
            destination_audio->time_base = audio_encoder_context->time_base;
            destination_audio->disposition = source_audio->disposition;
            status = avcodec_parameters_from_context(
                destination_audio->codecpar, audio_encoder_context.get());
            if (status < 0) {
                return Failure(
                    ErrorCategory::Codec,
                    "FFmpeg could not configure the output audio stream",
                    status);
            }
            if (request.video.preserve_metadata) {
                status = av_dict_copy(&destination_audio->metadata,
                                      source_audio->metadata, 0);
                if (status < 0) {
                    return Failure(ErrorCategory::Codec,
                                   "FFmpeg could not copy audio metadata",
                                   status);
                }
            }

            SwrContext* raw_resampler = nullptr;
            status = swr_alloc_set_opts2(
                &raw_resampler, &audio_encoder_context->ch_layout,
                audio_encoder_context->sample_fmt,
                audio_encoder_context->sample_rate,
                &audio_decoder_context->ch_layout,
                audio_decoder_context->sample_fmt,
                audio_decoder_context->sample_rate, 0, nullptr);
            audio_resampler.reset(raw_resampler);
            if (status >= 0 && audio_resampler) {
                status = swr_init(audio_resampler.get());
            }
            if (status < 0 || !audio_resampler) {
                return Failure(
                    ErrorCategory::Codec,
                    "FFmpeg could not initialize video audio resampling",
                    status);
            }
            const int frame_size = audio_encoder_context->frame_size > 0
                                       ? audio_encoder_context->frame_size
                                       : 1'024;
            audio_fifo.reset(av_audio_fifo_alloc(
                audio_encoder_context->sample_fmt,
                audio_encoder_context->ch_layout.nb_channels, frame_size * 2));
            if (!audio_fifo) {
                return ProviderOutcome::Failed(
                    ErrorCategory::ResourceLimit,
                    "FFmpeg could not allocate the bounded audio queue.");
            }
            stream_map[static_cast<std::size_t>(audio_index)] =
                destination_audio->index;
            selected_audio_codec = audio_encoder->name;
            transcode_audio = true;
        }
    }

    for (unsigned int index = 0; index < input.value->nb_streams; ++index) {
        AVStream* source = input.value->streams[index];
        if (source->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE ||
            request.video.subtitles != core::SubtitleHandling::CopyCompatible) {
            continue;
        }
        if (avformat_query_codec(output.value->oformat,
                                 source->codecpar->codec_id,
                                 FF_COMPLIANCE_NORMAL) <= 0) {
            ++discarded_streams;
            continue;
        }
        AVStream* destination = avformat_new_stream(output.value, nullptr);
        if (destination == nullptr) {
            return ProviderOutcome::Failed(
                ErrorCategory::ResourceLimit,
                "FFmpeg could not allocate an output subtitle stream.");
        }
        status =
            avcodec_parameters_copy(destination->codecpar, source->codecpar);
        if (status < 0) {
            return Failure(ErrorCategory::Codec,
                           "FFmpeg could not copy subtitle parameters", status);
        }
        destination->codecpar->codec_tag = 0;
        destination->time_base = source->time_base;
        stream_map[index] = destination->index;
    }

    for (unsigned int index = 0; index < input.value->nb_streams; ++index) {
        if (static_cast<int>(index) == video_index ||
            static_cast<int>(index) == audio_index || stream_map[index] >= 0) {
            continue;
        }
        const auto type = input.value->streams[index]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO ||
            type == AVMEDIA_TYPE_SUBTITLE) {
            ++discarded_streams;
        }
    }

    if (request.video.preserve_metadata) {
        status =
            av_dict_copy(&output.value->metadata, input.value->metadata, 0);
        if (status < 0) {
            return Failure(ErrorCategory::Codec,
                           "FFmpeg could not copy container metadata", status);
        }
    }
    if ((output.value->oformat->flags & AVFMT_NOFILE) == 0) {
        status =
            avio_open(&output.value->pb, output_name.c_str(), AVIO_FLAG_WRITE);
        if (status < 0) {
            return Failure(ErrorCategory::Io,
                           "FFmpeg could not open the temporary video output",
                           status);
        }
    }
    status = avformat_write_header(output.value, nullptr);
    if (status < 0) {
        return Failure(ErrorCategory::Codec,
                       "FFmpeg could not write the video header", status);
    }

    PacketPtr input_packet(av_packet_alloc());
    PacketPtr output_packet(av_packet_alloc());
    FramePtr decoded_frame(av_frame_alloc());
    PacketPtr audio_output_packet(transcode_audio ? av_packet_alloc()
                                                  : nullptr);
    FramePtr decoded_audio_frame(transcode_audio ? av_frame_alloc() : nullptr);
    if (!input_packet || !output_packet || !decoded_frame) {
        return ProviderOutcome::Failed(
            ErrorCategory::ResourceLimit,
            "FFmpeg could not allocate bounded video buffers.");
    }
    if (transcode_audio && (!audio_output_packet || !decoded_audio_frame)) {
        return ProviderOutcome::Failed(
            ErrorCategory::ResourceLimit,
            "FFmpeg could not allocate bounded audio buffers.");
    }
    SwsPtr scaler;
    int scaler_width = 0;
    int scaler_height = 0;
    AVPixelFormat scaler_format = AV_PIX_FMT_NONE;
    std::int64_t last_pts = AV_NOPTS_VALUE;
    std::int64_t next_audio_pts = 0;

    auto write_encoded_packets = [&]() -> std::optional<ProviderOutcome> {
        while (true) {
            status = avcodec_receive_packet(encoder.context.get(),
                                            output_packet.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return std::nullopt;
            }
            if (status < 0) {
                return Failure(ErrorCategory::Codec,
                               "The video encoder failed to produce a packet",
                               status);
            }
            av_packet_rescale_ts(output_packet.get(),
                                 encoder.context->time_base,
                                 destination_video->time_base);
            output_packet->stream_index = destination_video->index;
            status =
                av_interleaved_write_frame(output.value, output_packet.get());
            av_packet_unref(output_packet.get());
            if (status < 0) {
                return Failure(ErrorCategory::Io,
                               "FFmpeg could not write encoded video", status);
            }
        }
    };

    auto encode_frame =
        [&](const AVFrame* source) -> std::optional<ProviderOutcome> {
        const auto source_format = static_cast<AVPixelFormat>(source->format);
        if (!scaler || scaler_width != source->width ||
            scaler_height != source->height || scaler_format != source_format) {
            scaler.reset(sws_getContext(source->width, source->height,
                                        source_format, encoder.context->width,
                                        encoder.context->height,
                                        encoder.context->pix_fmt, SWS_BICUBIC,
                                        nullptr, nullptr, nullptr));
            scaler_width = source->width;
            scaler_height = source->height;
            scaler_format = source_format;
            if (!scaler) {
                return ProviderOutcome::Failed(
                    ErrorCategory::Codec,
                    "FFmpeg could not initialize video scaling.");
            }
        }
        FramePtr converted(av_frame_alloc());
        if (!converted) {
            return ProviderOutcome::Failed(
                ErrorCategory::ResourceLimit,
                "FFmpeg could not allocate an encoder frame.");
        }
        converted->format = encoder.context->pix_fmt;
        converted->width = encoder.context->width;
        converted->height = encoder.context->height;
        converted->sample_aspect_ratio = encoder.context->sample_aspect_ratio;
        converted->color_range = encoder.context->color_range;
        converted->color_primaries = encoder.context->color_primaries;
        converted->color_trc = encoder.context->color_trc;
        converted->colorspace = encoder.context->colorspace;
        status = av_frame_get_buffer(converted.get(), 32);
        if (status < 0) {
            return Failure(ErrorCategory::ResourceLimit,
                           "FFmpeg could not allocate scaled video data",
                           status);
        }
        status =
            sws_scale(scaler.get(), source->data, source->linesize, 0,
                      source->height, converted->data, converted->linesize);
        if (status <= 0) {
            return ProviderOutcome::Failed(
                ErrorCategory::Codec,
                "FFmpeg could not scale a decoded video frame.");
        }
        std::int64_t source_pts = source->best_effort_timestamp;
        if (source_pts == AV_NOPTS_VALUE) {
            source_pts = last_pts == AV_NOPTS_VALUE ? 0 : last_pts + 1;
        } else {
            source_pts = av_rescale_q(source_pts, source_video->time_base,
                                      encoder.context->time_base);
            if (last_pts != AV_NOPTS_VALUE && source_pts <= last_pts) {
                source_pts = last_pts + 1;
            }
        }
        converted->pts = source_pts;
        last_pts = source_pts;
        status = avcodec_send_frame(encoder.context.get(), converted.get());
        if (status < 0) {
            return Failure(ErrorCategory::Codec,
                           "The video encoder rejected a frame", status);
        }
        return write_encoded_packets();
    };

    auto drain_decoder = [&]() -> std::optional<ProviderOutcome> {
        while (true) {
            status = avcodec_receive_frame(decoder_context.get(),
                                           decoded_frame.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return std::nullopt;
            }
            if (status < 0) {
                return Failure(ErrorCategory::InvalidInput,
                               "The video decoder rejected the input stream",
                               status);
            }
            if (auto failure = encode_frame(decoded_frame.get())) {
                return failure;
            }
            if (progress && input.value->duration > 0 &&
                decoded_frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                const auto current = av_rescale_q(
                    decoded_frame->best_effort_timestamp,
                    source_video->time_base, AVRational{1, AV_TIME_BASE});
                const double fraction =
                    std::clamp(static_cast<double>(current) /
                                   static_cast<double>(input.value->duration),
                               0.0, 1.0);
                progress({0.05 + fraction * 0.90, "Transcoding video"});
            }
            av_frame_unref(decoded_frame.get());
        }
    };

    auto write_audio_packets = [&]() -> std::optional<ProviderOutcome> {
        while (true) {
            status = avcodec_receive_packet(audio_encoder_context.get(),
                                            audio_output_packet.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return std::nullopt;
            }
            if (status < 0) {
                return Failure(
                    ErrorCategory::Codec,
                    "The video audio encoder failed to produce a packet",
                    status);
            }
            av_packet_rescale_ts(audio_output_packet.get(),
                                 audio_encoder_context->time_base,
                                 destination_audio->time_base);
            audio_output_packet->stream_index = destination_audio->index;
            status = av_interleaved_write_frame(output.value,
                                                audio_output_packet.get());
            av_packet_unref(audio_output_packet.get());
            if (status < 0) {
                return Failure(ErrorCategory::Io,
                               "FFmpeg could not write encoded video audio",
                               status);
            }
        }
    };

    auto encode_available_audio =
        [&](const bool final) -> std::optional<ProviderOutcome> {
        const int frame_size = audio_encoder_context->frame_size;
        while (av_audio_fifo_size(audio_fifo.get()) >=
                   (frame_size > 0 ? frame_size : 1) ||
               (final && av_audio_fifo_size(audio_fifo.get()) > 0)) {
            const int available = av_audio_fifo_size(audio_fifo.get());
            const int samples =
                frame_size > 0 ? std::min(frame_size, available) : available;
            FramePtr frame(av_frame_alloc());
            if (!frame) {
                return ProviderOutcome::Failed(
                    ErrorCategory::ResourceLimit,
                    "FFmpeg could not allocate a video audio frame.");
            }
            frame->nb_samples = samples;
            frame->format = audio_encoder_context->sample_fmt;
            frame->sample_rate = audio_encoder_context->sample_rate;
            if (av_channel_layout_copy(&frame->ch_layout,
                                       &audio_encoder_context->ch_layout) < 0) {
                return ProviderOutcome::Failed(
                    ErrorCategory::ResourceLimit,
                    "FFmpeg could not copy the video audio channel layout.");
            }
            status = av_frame_get_buffer(frame.get(), 0);
            if (status < 0) {
                return Failure(
                    ErrorCategory::ResourceLimit,
                    "FFmpeg could not allocate video audio sample data",
                    status);
            }
            if (av_audio_fifo_read(
                    audio_fifo.get(),
                    reinterpret_cast<void**>(frame->extended_data),
                    samples) != samples) {
                return ProviderOutcome::Failed(
                    ErrorCategory::Codec,
                    "FFmpeg could not read the resampled video audio buffer.");
            }
            frame->pts = next_audio_pts;
            next_audio_pts += samples;
            status =
                avcodec_send_frame(audio_encoder_context.get(), frame.get());
            if (status < 0) {
                return Failure(ErrorCategory::Codec,
                               "The video audio encoder rejected a frame",
                               status);
            }
            if (auto failure = write_audio_packets()) {
                return failure;
            }
        }
        return std::nullopt;
    };

    auto queue_resampled_audio =
        [&](const AVFrame* frame) -> std::optional<ProviderOutcome> {
        const std::int64_t delayed = swr_get_delay(
            audio_resampler.get(), audio_decoder_context->sample_rate);
        const auto capacity64 = av_rescale_rnd(
            delayed + frame->nb_samples, audio_encoder_context->sample_rate,
            audio_decoder_context->sample_rate, AV_ROUND_UP);
        if (capacity64 <= 0 || capacity64 > std::numeric_limits<int>::max()) {
            return ProviderOutcome::Failed(
                ErrorCategory::ResourceLimit,
                "The resampled video audio buffer exceeds the safe limit.");
        }
        FramePtr converted(av_frame_alloc());
        if (!converted) {
            return ProviderOutcome::Failed(
                ErrorCategory::ResourceLimit,
                "FFmpeg could not allocate a video audio resampling frame.");
        }
        converted->nb_samples = static_cast<int>(capacity64);
        converted->format = audio_encoder_context->sample_fmt;
        converted->sample_rate = audio_encoder_context->sample_rate;
        if (av_channel_layout_copy(&converted->ch_layout,
                                   &audio_encoder_context->ch_layout) < 0) {
            return ProviderOutcome::Failed(
                ErrorCategory::ResourceLimit,
                "FFmpeg could not copy the resampling channel layout.");
        }
        status = av_frame_get_buffer(converted.get(), 0);
        if (status < 0) {
            return Failure(
                ErrorCategory::ResourceLimit,
                "FFmpeg could not allocate resampled video audio data", status);
        }
        status =
            swr_convert(audio_resampler.get(), converted->extended_data,
                        converted->nb_samples,
                        const_cast<const std::uint8_t**>(frame->extended_data),
                        frame->nb_samples);
        if (status < 0) {
            return Failure(ErrorCategory::Codec,
                           "FFmpeg could not resample video audio", status);
        }
        if (status == 0) {
            return std::nullopt;
        }
        if (av_audio_fifo_realloc(audio_fifo.get(),
                                  av_audio_fifo_size(audio_fifo.get()) +
                                      status) < 0 ||
            av_audio_fifo_write(
                audio_fifo.get(),
                reinterpret_cast<void**>(converted->extended_data),
                status) != status) {
            return ProviderOutcome::Failed(
                ErrorCategory::ResourceLimit,
                "FFmpeg could not queue resampled video audio.");
        }
        return encode_available_audio(false);
    };

    auto drain_audio_decoder = [&]() -> std::optional<ProviderOutcome> {
        while (true) {
            status = avcodec_receive_frame(audio_decoder_context.get(),
                                           decoded_audio_frame.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return std::nullopt;
            }
            if (status < 0) {
                return Failure(
                    ErrorCategory::InvalidInput,
                    "The video audio decoder rejected the input stream",
                    status);
            }
            if (auto failure =
                    queue_resampled_audio(decoded_audio_frame.get())) {
                return failure;
            }
            av_frame_unref(decoded_audio_frame.get());
        }
    };

    while (!cancellation.stop_requested() &&
           (status = av_read_frame(input.value, input_packet.get())) >= 0) {
        const int source_index = input_packet->stream_index;
        if (source_index == video_index) {
            status =
                avcodec_send_packet(decoder_context.get(), input_packet.get());
            av_packet_unref(input_packet.get());
            if (status < 0) {
                return Failure(ErrorCategory::InvalidInput,
                               "The video decoder rejected an input packet",
                               status);
            }
            if (auto failure = drain_decoder()) {
                return *failure;
            }
            continue;
        }
        if (transcode_audio && source_index == audio_index) {
            status = avcodec_send_packet(audio_decoder_context.get(),
                                         input_packet.get());
            av_packet_unref(input_packet.get());
            if (status < 0) {
                return Failure(
                    ErrorCategory::InvalidInput,
                    "The video audio decoder rejected an input packet", status);
            }
            if (auto failure = drain_audio_decoder()) {
                return *failure;
            }
            continue;
        }
        if (source_index < 0 ||
            static_cast<std::size_t>(source_index) >= stream_map.size() ||
            stream_map[static_cast<std::size_t>(source_index)] < 0) {
            av_packet_unref(input_packet.get());
            continue;
        }
        AVStream* source = input.value->streams[source_index];
        AVStream* destination =
            output.value
                ->streams[stream_map[static_cast<std::size_t>(source_index)]];
        av_packet_rescale_ts(input_packet.get(), source->time_base,
                             destination->time_base);
        input_packet->stream_index = destination->index;
        input_packet->pos = -1;
        status = av_interleaved_write_frame(output.value, input_packet.get());
        av_packet_unref(input_packet.get());
        if (status < 0) {
            return Failure(ErrorCategory::Io,
                           "FFmpeg could not copy an auxiliary stream packet",
                           status);
        }
    }
    if (cancellation.stop_requested()) {
        return ProviderOutcome::Cancelled("Video conversion cancelled.");
    }
    if (status != AVERROR_EOF) {
        return Failure(ErrorCategory::InvalidInput,
                       "FFmpeg could not read the complete video input",
                       status);
    }

    status = avcodec_send_packet(decoder_context.get(), nullptr);
    if (status < 0) {
        return Failure(ErrorCategory::Codec,
                       "FFmpeg could not flush the video decoder", status);
    }
    if (auto failure = drain_decoder()) {
        return *failure;
    }
    status = avcodec_send_frame(encoder.context.get(), nullptr);
    if (status < 0) {
        return Failure(ErrorCategory::Codec,
                       "FFmpeg could not flush the video encoder", status);
    }
    if (auto failure = write_encoded_packets()) {
        return *failure;
    }
    if (transcode_audio) {
        status = avcodec_send_packet(audio_decoder_context.get(), nullptr);
        if (status < 0) {
            return Failure(ErrorCategory::Codec,
                           "FFmpeg could not flush the video audio decoder",
                           status);
        }
        if (auto failure = drain_audio_decoder()) {
            return *failure;
        }

        while (swr_get_delay(audio_resampler.get(),
                             audio_decoder_context->sample_rate) > 0) {
            const auto capacity64 = av_rescale_rnd(
                swr_get_delay(audio_resampler.get(),
                              audio_decoder_context->sample_rate),
                audio_encoder_context->sample_rate,
                audio_decoder_context->sample_rate, AV_ROUND_UP);
            if (capacity64 <= 0 ||
                capacity64 > std::numeric_limits<int>::max()) {
                break;
            }
            FramePtr converted(av_frame_alloc());
            if (!converted) {
                return ProviderOutcome::Failed(
                    ErrorCategory::ResourceLimit,
                    "FFmpeg could not allocate final video audio data.");
            }
            converted->nb_samples = static_cast<int>(capacity64);
            converted->format = audio_encoder_context->sample_fmt;
            converted->sample_rate = audio_encoder_context->sample_rate;
            if (av_channel_layout_copy(&converted->ch_layout,
                                       &audio_encoder_context->ch_layout) < 0 ||
                av_frame_get_buffer(converted.get(), 0) < 0) {
                return ProviderOutcome::Failed(
                    ErrorCategory::ResourceLimit,
                    "FFmpeg could not allocate final resampled video audio.");
            }
            const int samples =
                swr_convert(audio_resampler.get(), converted->extended_data,
                            converted->nb_samples, nullptr, 0);
            if (samples <= 0) {
                break;
            }
            if (av_audio_fifo_realloc(audio_fifo.get(),
                                      av_audio_fifo_size(audio_fifo.get()) +
                                          samples) < 0 ||
                av_audio_fifo_write(
                    audio_fifo.get(),
                    reinterpret_cast<void**>(converted->extended_data),
                    samples) != samples) {
                return ProviderOutcome::Failed(
                    ErrorCategory::ResourceLimit,
                    "FFmpeg could not queue final resampled video audio.");
            }
        }
        if (auto failure = encode_available_audio(true)) {
            return *failure;
        }
        status = avcodec_send_frame(audio_encoder_context.get(), nullptr);
        if (status < 0) {
            return Failure(ErrorCategory::Codec,
                           "FFmpeg could not flush the video audio encoder",
                           status);
        }
        if (auto failure = write_audio_packets()) {
            return *failure;
        }
    }
    status = av_write_trailer(output.value);
    if (status < 0) {
        return Failure(ErrorCategory::Io,
                       "FFmpeg could not finish the video container", status);
    }

    auto outcome = ProviderOutcome::Succeeded();
    outcome.warnings = std::move(encoder.warnings);
    outcome.selected_codec = encoder.name;
    outcome.hardware_acceleration =
        IsHardwareEncoderName(encoder.name) ? encoder.name : "software";
    if (transcode_audio) {
        outcome.warnings.push_back("Audio transcoded with " +
                                   selected_audio_codec + ".");
    }
    if (discarded_streams > 0) {
        outcome.warnings.push_back(
            std::to_string(discarded_streams) +
            " additional or incompatible stream(s) were not copied.");
    }
    outcome.warnings.push_back("Video encoded with " + encoder.name + ".");
    return outcome;
}

} // namespace

ProviderOutcome TranscodeVideoEncoded(const core::ConversionRequest& request,
                                      const core::ProgressCallback& progress,
                                      const std::stop_token cancellation) {
    return EncodeVideo(request, progress, cancellation);
}

} // namespace nativeshift::media
