#include "media_operations.hpp"

#include "ffmpeg_support.hpp"

extern "C" {
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

namespace nativeshift::media {
namespace {

using core::ErrorCategory;
using core::ProviderOutcome;
using detail::InputContext;
using detail::OutputContext;
using detail::PacketPtr;

struct InterruptState {
    std::stop_token cancellation;
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

bool IsSelectedStream(const core::ConversionRequest& request,
                      const AVStream* stream, const int video_index,
                      const int audio_index) {
    switch (stream->codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        return stream->index == video_index;
    case AVMEDIA_TYPE_AUDIO:
        return request.video.audio_codec != core::VideoAudioCodec::None &&
               stream->index == audio_index;
    case AVMEDIA_TYPE_SUBTITLE:
        return request.video.subtitles ==
               core::SubtitleHandling::CopyCompatible;
    default:
        return false;
    }
}

ProviderOutcome Remux(const core::ConversionRequest& request,
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

    std::vector<int> stream_map(input.value->nb_streams, -1);
    int selected_streams = 0;
    int discarded_streams = 0;
    for (unsigned int index = 0; index < input.value->nb_streams; ++index) {
        AVStream* source = input.value->streams[index];
        if (!IsSelectedStream(request, source, video_index, audio_index)) {
            if (source->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
                source->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ||
                source->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                ++discarded_streams;
            }
            continue;
        }
        if (avformat_query_codec(output.value->oformat,
                                 source->codecpar->codec_id,
                                 FF_COMPLIANCE_NORMAL) <= 0) {
            if (source->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                ++discarded_streams;
                continue;
            }
            return ProviderOutcome::Failed(
                ErrorCategory::UnsupportedFormat,
                "A selected source codec cannot be copied into the requested "
                "video container.");
        }

        AVStream* destination = avformat_new_stream(output.value, nullptr);
        if (destination == nullptr) {
            return ProviderOutcome::Failed(
                ErrorCategory::ResourceLimit,
                "FFmpeg could not allocate an output stream.");
        }
        status =
            avcodec_parameters_copy(destination->codecpar, source->codecpar);
        if (status < 0) {
            return Failure(ErrorCategory::Codec,
                           "FFmpeg could not copy stream parameters", status);
        }
        destination->codecpar->codec_tag = 0;
        destination->time_base = source->time_base;
        destination->avg_frame_rate = source->avg_frame_rate;
        destination->sample_aspect_ratio = source->sample_aspect_ratio;
        destination->disposition = source->disposition;
        status = av_dict_copy(&destination->metadata, source->metadata, 0);
        if (status < 0) {
            return Failure(ErrorCategory::Codec,
                           "FFmpeg could not copy stream metadata", status);
        }
        stream_map[index] = destination->index;
        ++selected_streams;
    }
    if (selected_streams == 0) {
        return ProviderOutcome::Failed(
            ErrorCategory::UnsupportedFormat,
            "No compatible streams were selected for the output container.");
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

    PacketPtr packet(av_packet_alloc());
    if (!packet) {
        return ProviderOutcome::Failed(
            ErrorCategory::ResourceLimit,
            "FFmpeg could not allocate the bounded packet buffer.");
    }
    while (!cancellation.stop_requested() &&
           (status = av_read_frame(input.value, packet.get())) >= 0) {
        const int source_index = packet->stream_index;
        if (source_index < 0 ||
            static_cast<std::size_t>(source_index) >= stream_map.size() ||
            stream_map[static_cast<std::size_t>(source_index)] < 0) {
            av_packet_unref(packet.get());
            continue;
        }
        AVStream* source = input.value->streams[source_index];
        AVStream* destination =
            output.value
                ->streams[stream_map[static_cast<std::size_t>(source_index)]];
        const std::int64_t progress_pts =
            packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        av_packet_rescale_ts(packet.get(), source->time_base,
                             destination->time_base);
        packet->stream_index = destination->index;
        packet->pos = -1;
        status = av_interleaved_write_frame(output.value, packet.get());
        av_packet_unref(packet.get());
        if (status < 0) {
            return Failure(ErrorCategory::Io,
                           "FFmpeg could not write a remuxed packet", status);
        }
        if (progress && input.value->duration > 0 &&
            progress_pts != AV_NOPTS_VALUE) {
            const auto current = av_rescale_q(progress_pts, source->time_base,
                                              AVRational{1, AV_TIME_BASE});
            const double fraction =
                std::clamp(static_cast<double>(current) /
                               static_cast<double>(input.value->duration),
                           0.0, 1.0);
            progress({0.05 + fraction * 0.90, "Remuxing video streams"});
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
    status = av_write_trailer(output.value);
    if (status < 0) {
        return Failure(ErrorCategory::Io,
                       "FFmpeg could not finish the video container", status);
    }

    auto outcome = ProviderOutcome::Succeeded();
    outcome.selected_codec = "stream-copy";
    if (discarded_streams > 0) {
        outcome.warnings.push_back(
            std::to_string(discarded_streams) +
            " additional or incompatible stream(s) were not copied; the "
            "default selected video/audio streams were retained.");
    }
    return outcome;
}

} // namespace

ProviderOutcome TranscodeVideo(const core::ConversionRequest& request,
                               const core::ProgressCallback& progress,
                               const std::stop_token cancellation) {
    if (!request.video.stream_copy &&
        request.video.video_codec != core::VideoCodec::Copy) {
        return TranscodeVideoEncoded(request, progress, cancellation);
    }
    return Remux(request, progress, cancellation);
}

} // namespace nativeshift::media
