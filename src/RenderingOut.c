#include "RenderingOut.h"

#include "D3D11FrameConverter.h"
#include "Unity.h"

#include <errno.h>
#include <objbase.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>

typedef struct EncoderCandidate
{
	const char* name;
	enum AVPixelFormat input_format;
	int direct_d3d11;
} EncoderCandidate;

typedef enum EncoderWorkerCommand
{
	ENCODER_COMMAND_NONE,
	ENCODER_COMMAND_INITIALIZE,
	ENCODER_COMMAND_ENCODE,
	ENCODER_COMMAND_FINISH,
	ENCODER_COMMAND_EXIT
} EncoderWorkerCommand;

struct VideoEncoder
{
	AVCodecContext* ctx;
	AVCodecContext* audio_ctx;
	AVFormatContext* format;
	AVStream* stream;
	AVStream* audio_stream;
	AVPacket* packet;
	AVFrame* audio_frame;
	AVBufferRef* hw_frames;
	D3D11FrameConverter* converter;
	char* filename;
	uint8_t* pending_pcm_data;
	int64_t pending_pcm_size;
	int pending_pcm_sample_rate;
	int pending_pcm_channels;

	HANDLE worker;
	CRITICAL_SECTION worker_lock;
	CONDITION_VARIABLE worker_changed;
	EncoderWorkerCommand worker_command;
	AVFrame* worker_frame;
	enum AVPixelFormat worker_texture_format;
	int worker_multisampled;
	int worker_result;

	int bit_rate;
	int width;
	int height;
	int fps;
	int initialized;
	int header_written;
	int fatal_error;
	int direct_d3d11;
	enum AVPixelFormat texture_format;
	enum AVPixelFormat input_format;
	int64_t next_pts;
	int64_t audio_next_pts;
	int audio_enabled;
};

static int write_available_packets(
	VideoEncoder* ve,
	AVCodecContext* ctx,
	AVStream* stream)
{
	int ret;

	if (!ve->header_written || !ctx || !stream)
		return AVERROR(EINVAL);

	for (;;)
	{
		ret = avcodec_receive_packet(ctx, ve->packet);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return 0;
		if (ret < 0)
			return ret;

		av_packet_rescale_ts(
			ve->packet,
			ctx->time_base,
			stream->time_base);
		ve->packet->stream_index = stream->index;

		ret = av_interleaved_write_frame(ve->format, ve->packet);
		av_packet_unref(ve->packet);
		if (ret < 0)
			return ret;
	}
}

static int create_hw_frames(
        VideoEncoder* ve,
        enum AVPixelFormat software_format,
        UINT bind_flags,
        AVBufferRef** frames)
{
        AVBufferRef* result;
        AVHWFramesContext* context;
        AVD3D11VAFramesContext* d3d11_context;
        int ret;

	if (!renderingout_get_hw_device())
		return AVERROR(ENODEV);

	result = av_hwframe_ctx_alloc(renderingout_get_hw_device());
	if (!result)
		return AVERROR(ENOMEM);

	context = (AVHWFramesContext*)result->data;
	context->format = AV_PIX_FMT_D3D11;
	context->sw_format = software_format;
	context->width = ve->width;
	context->height = ve->height;
	d3d11_context = (AVD3D11VAFramesContext*)context->hwctx;
	d3d11_context->BindFlags = bind_flags;

        /*
         * D3D11 hardware frames are texture-array slices. The fixed pool lets the
         * encoder retain a frame without retaining Unity's render target.
         */
        context->initial_pool_size = 16;

        ret = av_hwframe_ctx_init(result);
        if (ret < 0)
	{
		av_buffer_unref(&result);
                return ret;
        }

        *frames = result;
        return 0;
}

static void set_codec_options(
        const EncoderCandidate* candidate,
        AVDictionary** options)
{
	if (strcmp(candidate->name, "libx264") == 0)
	{
		av_dict_set(options, "preset", "veryfast", 0);
		av_dict_set(options, "tune", "zerolatency", 0);
	}
	else if (strcmp(candidate->name, "h264_mf") == 0)
	{
		if (candidate->direct_d3d11)
			av_dict_set(options, "hw_encoding", "1", 0);
	}
	else if (strcmp(candidate->name, "h264_nvenc") == 0)
	{
		av_dict_set(options, "delay", "0", 0);
		av_dict_set(options, "zerolatency", "1", 0);
        }
}

static const char* encoder_candidate_mode(const EncoderCandidate* candidate)
{
        return candidate->direct_d3d11 ? "direct" : "readback";
}

static void append_encoder_attempt(
        char* summary,
        size_t summary_size,
        const char* attempt)
{
        size_t length;

        if (!summary || summary_size == 0 || !attempt)
                return;

        length = strlen(summary);
        if (length >= summary_size - 1)
                return;

        snprintf(
                summary + length,
                summary_size - length,
                "%s%s",
                length > 0 ? "; " : "",
                attempt);
}

static int try_open_encoder(
        VideoEncoder* ve,
        const EncoderCandidate* candidate,
        char* attempt,
        size_t attempt_size)
{
	const AVCodec* codec;
	AVCodecContext* context = NULL;
        AVBufferRef* frames = NULL;
        AVDictionary* options = NULL;
        int ret;

        snprintf(
                attempt,
                attempt_size,
                "%s/%s=failed",
                candidate->name,
                encoder_candidate_mode(candidate));

        codec = avcodec_find_encoder_by_name(candidate->name);
        if (!codec)
        {
                snprintf(
                        attempt,
                        attempt_size,
                        "%s/%s=unavailable",
                        candidate->name,
                        encoder_candidate_mode(candidate));
                return AVERROR_ENCODER_NOT_FOUND;
        }

	context = avcodec_alloc_context3(codec);
	if (!context)
		return AVERROR(ENOMEM);

	context->bit_rate = ve->bit_rate;
	context->width = ve->width;
	context->height = ve->height;
	context->framerate = (AVRational){ ve->fps, 1 };
	context->time_base = (AVRational){ 1, ve->fps };
	context->gop_size = ve->fps;
	context->max_b_frames = 0;

	if (strcmp(candidate->name, "h264_mf") != 0)
		context->flags |= AV_CODEC_FLAG_LOW_DELAY;

	context->pix_fmt = candidate->direct_d3d11
		? AV_PIX_FMT_D3D11
		: candidate->input_format;
	context->color_range = AVCOL_RANGE_MPEG;

	if (ve->height >= 720)
	{
		context->colorspace = AVCOL_SPC_BT709;
		context->color_primaries = AVCOL_PRI_BT709;
		context->color_trc = AVCOL_TRC_BT709;
	}
	else
	{
		context->colorspace = AVCOL_SPC_SMPTE170M;
		context->color_primaries = AVCOL_PRI_SMPTE170M;
		context->color_trc = AVCOL_TRC_SMPTE170M;
	}

	if (ve->format->oformat->flags & AVFMT_GLOBALHEADER)
		context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        if (candidate->direct_d3d11)
        {
                UINT bind_flags = 0;

                /*
                 * Media Foundation consumes NV12 video-encoder surfaces. The render
                 * target flag additionally lets the D3D11 video processor write the
                 * RGB-to-NV12 result directly into the encoder-owned frame.
                 */
                if (strcmp(candidate->name, "h264_mf") == 0)
                        bind_flags =
                                D3D11_BIND_VIDEO_ENCODER |
                                D3D11_BIND_RENDER_TARGET;

                ret = create_hw_frames(
                        ve,
                        candidate->input_format,
                        bind_flags,
                        &frames);
                if (ret < 0)
                {
                        snprintf(
                                attempt,
                                attempt_size,
                                "%s/direct=hw-frames failed (%s)",
                                candidate->name, av_err2str(ret));
                        goto fail;
		}

		context->hw_frames_ctx = av_buffer_ref(frames);
		if (!context->hw_frames_ctx)
		{
			ret = AVERROR(ENOMEM);
			goto fail;
		}
	}

	set_codec_options(candidate, &options);
	ret = avcodec_open2(context, codec, &options);
	av_dict_free(&options);
        if (ret < 0)
        {
                snprintf(
                        attempt,
                        attempt_size,
                        "%s/%s=open failed (%s)",
                        candidate->name,
                        encoder_candidate_mode(candidate),
                        av_err2str(ret));
                goto fail;
	}

	ve->ctx = context;
	ve->hw_frames = frames;
        ve->input_format = candidate->input_format;
        ve->direct_d3d11 = candidate->direct_d3d11;

        snprintf(
                attempt,
                attempt_size,
                "%s/%s=selected",
                candidate->name,
                encoder_candidate_mode(candidate));
        return 0;

fail:
	av_dict_free(&options);
	av_buffer_unref(&frames);
	avcodec_free_context(&context);
	return ret;
}

static int initialize_encoder(
        VideoEncoder* ve,
        enum AVPixelFormat texture_format,
        int multisampled)
{
        EncoderCandidate candidates[8];
        char selection_summary[1536] = { 0 };
        char attempt[256];
        int candidate_count = 0;
        int mf_direct_ready = 0;
        int i;
        int last_error = AVERROR_ENCODER_NOT_FOUND;

        renderingout_set_probe_logging_suppressed(1);

        if (!multisampled)
	{
		DXGI_FORMAT source_format = DXGI_FORMAT_UNKNOWN;
		int ret;

		candidates[candidate_count++] =
			(EncoderCandidate){ "h264_nvenc", texture_format, 1 };

		switch (texture_format)
		{
		case AV_PIX_FMT_NV12:
			source_format = DXGI_FORMAT_NV12;
			break;
		case AV_PIX_FMT_BGRA:
			source_format = DXGI_FORMAT_B8G8R8A8_UNORM;
			break;
		case AV_PIX_FMT_RGBA:
			source_format = DXGI_FORMAT_R8G8B8A8_UNORM;
			break;
		default:
			break;
		}

		ret = d3d11_frame_prepare_nv12_conversion(
			ve->converter,
			source_format);
                if (ret >= 0)
                        mf_direct_ready = 1;
                else
                {
                        snprintf(
                                attempt,
                                sizeof(attempt),
                                "h264_mf/direct=preflight failed for %s (%s)",
                                av_get_pix_fmt_name(texture_format),
                                av_err2str(ret));
                        append_encoder_attempt(
                                selection_summary,
                                sizeof(selection_summary),
                                attempt);
                }

		if (mf_direct_ready)
			candidates[candidate_count++] =
				(EncoderCandidate){ "h264_mf", AV_PIX_FMT_NV12, 1 };
	}

	candidates[candidate_count++] =
		(EncoderCandidate){ "h264_nvenc", texture_format, 0 };
	candidates[candidate_count++] =
		(EncoderCandidate){ "h264_mf", AV_PIX_FMT_NV12, 0 };
	candidates[candidate_count++] =
		(EncoderCandidate){ "h264_amf", AV_PIX_FMT_NV12, 0 };
	candidates[candidate_count++] =
		(EncoderCandidate){ "h264_qsv", AV_PIX_FMT_NV12, 0 };
	candidates[candidate_count++] =
		(EncoderCandidate){ "libx264", AV_PIX_FMT_NV12, 0 };

        for (i = 0; i < candidate_count; ++i)
        {
                last_error = try_open_encoder(
                        ve,
                        &candidates[i],
                        attempt,
                        sizeof(attempt));
                append_encoder_attempt(
                        selection_summary,
                        sizeof(selection_summary),
                        attempt);
                if (last_error >= 0)
                {
                        ve->initialized = 1;
                        ve->texture_format = texture_format;
                        renderingout_set_probe_logging_suppressed(0);
                        renderingout_logf(
                                "RenderingOut: encoder selection [%s]; "
                                "input=%s, %dx%d @ %d fps",
                                selection_summary,
                                av_get_pix_fmt_name(
                                        candidates[i].input_format),
                                ve->width,
                                ve->height,
                                ve->fps);
                        return 0;
                }
        }

        renderingout_set_probe_logging_suppressed(0);
        renderingout_log_errorf(
                "RenderingOut: encoder selection failed [%s]; final error: %s",
                selection_summary,
                av_err2str(last_error));
	return last_error;
}

static int initialize_audio_stream(VideoEncoder* ve)
{
	const AVCodec* codec;
	AVCodecContext* ctx = NULL;
	AVStream* stream = NULL;
	AVFrame* frame = NULL;
	int ret;

	/*
	 * AAC is the safe default for mp4/mov/webm/matroska. If the container
	 * cannot hold AAC the audio track is silently skipped, leaving a
	 * video-only file.
	 */
	codec = avcodec_find_encoder_by_name("aac");
	if (!codec)
	{
		renderingout_log_errorf("RenderingOut: AAC codec not found");
		return AVERROR_ENCODER_NOT_FOUND;
	}

	stream = avformat_new_stream(ve->format, codec);
	if (!stream)
		return AVERROR(ENOMEM);

	ctx = avcodec_alloc_context3(codec);
	if (!ctx)
	{
		ret = AVERROR(ENOMEM);
		goto fail;
	}

	ctx->bit_rate = 128000;
	ctx->sample_rate = 44100;
	ctx->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
	ctx->time_base = (AVRational){ 1, ctx->sample_rate };
	ctx->sample_fmt = codec->sample_fmts
		? codec->sample_fmts[0]
		: AV_SAMPLE_FMT_FLTP;

	if (ve->format->oformat->flags & AVFMT_GLOBALHEADER)
		ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	ret = avcodec_open2(ctx, codec, NULL);
	if (ret < 0)
		goto fail;

	frame = av_frame_alloc();
	if (!frame)
	{
		ret = AVERROR(ENOMEM);
		goto fail;
	}
	frame->format = ctx->sample_fmt;
	frame->sample_rate = ctx->sample_rate;
	av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
	frame->nb_samples = ctx->frame_size > 0 ? ctx->frame_size : 1024;
	ret = av_frame_get_buffer(frame, 0);
	if (ret < 0)
		goto fail;

	ret = avcodec_parameters_from_context(stream->codecpar, ctx);
	if (ret < 0)
		goto fail;
	stream->time_base = ctx->time_base;

	ve->audio_ctx = ctx;
	ve->audio_stream = stream;
	ve->audio_frame = frame;
	ve->audio_enabled = 1;

	renderingout_logf(
		"RenderingOut: audio stream initialized (codec: aac, %d Hz, %d channels, bitrate: %d bps, %d samples/frame)",
		ctx->sample_rate,
		ctx->ch_layout.nb_channels,
		ctx->bit_rate,
		frame->nb_samples);
	return 0;

fail:
	renderingout_log_errorf(
		"RenderingOut: audio stream initialization failed (%s)",
		av_err2str(ret));
	av_frame_free(&frame);
	avcodec_free_context(&ctx);
	return ret;
}

static int initialize_output(VideoEncoder* ve)
{
	AVDictionary* options = NULL;
	int ret;

	ve->stream = avformat_new_stream(ve->format, NULL);
	if (!ve->stream)
		return AVERROR(ENOMEM);

	ve->stream->time_base = ve->ctx->time_base;
	ve->stream->avg_frame_rate = ve->ctx->framerate;
	ret = avcodec_parameters_from_context(ve->stream->codecpar, ve->ctx);
	if (ret < 0)
		return ret;

	/*
	 * Audio is set up before the header is written so the stream appears in
	 * the file header. Failure here only disables the audio track; video
	 * still works.
	 */
	ret = initialize_audio_stream(ve);
	if (ret < 0)
	{
		renderingout_log_errorf(
			"RenderingOut: audio stream init failed (%s), fallback to video-only",
			av_err2str(ret));
		ve->audio_enabled = 0;
	}

	if (!(ve->format->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open(&ve->format->pb, ve->filename, AVIO_FLAG_WRITE);
		if (ret < 0)
		{
			renderingout_log_errorf(
				"RenderingOut: avio_open failed for '%s' (%s)",
				ve->filename, av_err2str(ret));
			return ret;
		}
	}

	if (strcmp(ve->format->oformat->name, "mp4") == 0 ||
		strcmp(ve->format->oformat->name, "mov") == 0)
		av_dict_set(&options, "movflags", "+faststart", 0);

	ret = avformat_write_header(ve->format, &options);
	av_dict_free(&options);
	if (ret < 0)
	{
		renderingout_log_errorf(
			"RenderingOut: avformat_write_header failed (%s)",
			av_err2str(ret));
		return ret;
	}

	ve->header_written = 1;
	renderingout_logf(
		"RenderingOut: header written for '%s' (Format: %s, Video Stream: #%d, Audio Stream: %s)",
		ve->filename,
		ve->format->oformat->name,
		ve->stream ? ve->stream->index : -1,
		ve->audio_enabled ? "Enabled (#1)" : "Disabled");
	return 0;
}

static int send_frame_and_write_packets(VideoEncoder* ve, AVFrame* frame)
{
	int ret;

	ret = avcodec_send_frame(ve->ctx, frame);
	if (ret == AVERROR(EAGAIN))
	{
		ret = write_available_packets(ve, ve->ctx, ve->stream);
		if (ret < 0)
			return ret;
		ret = avcodec_send_frame(ve->ctx, frame);
	}
	if (ret < 0)
		return ret;

	return write_available_packets(ve, ve->ctx, ve->stream);
}

static int flush_encoder(VideoEncoder* ve)
{
	int ret;

	if (!ve->initialized || !ve->header_written)
		return 0;

	ret = avcodec_send_frame(ve->ctx, NULL);
	if (ret == AVERROR(EAGAIN))
	{
		ret = write_available_packets(ve, ve->ctx, ve->stream);
		if (ret < 0)
			return ret;
		ret = avcodec_send_frame(ve->ctx, NULL);
	}
	if (ret < 0 && ret != AVERROR_EOF)
		return ret;

	return write_available_packets(ve, ve->ctx, ve->stream);
}

static int write_audio_frame(VideoEncoder* ve)
{
	int ret;

	ve->audio_frame->pts = ve->audio_next_pts;
	ve->audio_next_pts += ve->audio_frame->nb_samples;

	ret = avcodec_send_frame(ve->audio_ctx, ve->audio_frame);
	if (ret == AVERROR(EAGAIN))
	{
		ret = write_available_packets(ve, ve->audio_ctx, ve->audio_stream);
		if (ret < 0)
			return ret;
		ret = avcodec_send_frame(ve->audio_ctx, ve->audio_frame);
	}
	if (ret < 0)
		return ret;

	return write_available_packets(ve, ve->audio_ctx, ve->audio_stream);
}

static int flush_audio_encoder(VideoEncoder* ve)
{
	int ret;

	if (!ve->audio_enabled || !ve->audio_ctx || !ve->audio_stream)
		return 0;

	ret = avcodec_send_frame(ve->audio_ctx, NULL);
	if (ret == AVERROR(EAGAIN))
	{
		ret = write_available_packets(ve, ve->audio_ctx, ve->audio_stream);
		if (ret < 0)
			return ret;
		ret = avcodec_send_frame(ve->audio_ctx, NULL);
	}
	if (ret < 0 && ret != AVERROR_EOF)
		return ret;

	return write_available_packets(ve, ve->audio_ctx, ve->audio_stream);
}

/*
 * Resamples interleaved float PCM samples from the Unity caller into the
 * encoder's frame format (FLTP, stereo, 44100 Hz) and writes them to the
 * audio encoder. Uses an internal converter frame of encoder's frame_size;
 * swr_convert handles any leftover samples through its internal buffering.
 * If the source is shorter than the video the caller pads the remainder
 * with silence.
 */
static int write_audio_from_pcm(
	VideoEncoder* ve,
	const uint8_t* pcm_data,
	int64_t pcm_size,
	int sample_rate,
	int channels)
{
	SwrContext* resampler = NULL;
	AVFrame* converted = NULL;
	AVChannelLayout src_layout = { 0 };
	int frame_size;
	int src_bytes_per_sample;
	int64_t src_total_samples;
	int64_t src_offset;
	int ret;

	if (!pcm_data || pcm_size <= 0 || sample_rate <= 0 ||
		channels <= 0 || channels > AV_NUM_DATA_POINTERS)
		return AVERROR(EINVAL);

	src_bytes_per_sample = channels * sizeof(float);
	src_total_samples = pcm_size / src_bytes_per_sample;
	if (src_total_samples <= 0)
		return AVERROR(EINVAL);

	av_channel_layout_default(&src_layout, channels);

	ret = swr_alloc_set_opts2(
		&resampler,
		&ve->audio_ctx->ch_layout,
		ve->audio_ctx->sample_fmt,
		ve->audio_ctx->sample_rate,
		&src_layout,
		AV_SAMPLE_FMT_FLT,
		sample_rate,
		0, NULL);
	if (ret < 0)
		goto fail;
	ret = swr_init(resampler);
	if (ret < 0)
		goto fail;

	frame_size = ve->audio_ctx->frame_size > 0
		? ve->audio_ctx->frame_size
		: 1024;

	converted = av_frame_alloc();
	if (!converted)
	{
		ret = AVERROR(ENOMEM);
		goto fail;
	}

	src_offset = 0;
	while (src_offset < src_total_samples)
	{
		int64_t chunk = src_total_samples - src_offset;
		if (chunk > frame_size)
			chunk = frame_size;
		int in_samples = (int)chunk;

		av_frame_unref(converted);
		converted->format = ve->audio_ctx->sample_fmt;
		converted->sample_rate = ve->audio_ctx->sample_rate;
		av_channel_layout_copy(&converted->ch_layout, &ve->audio_ctx->ch_layout);
		converted->nb_samples = frame_size;

		ret = av_frame_get_buffer(converted, 0);
		if (ret < 0)
			goto fail;

		const uint8_t* src_ptr = pcm_data +
			src_offset * src_bytes_per_sample;
		ret = swr_convert(
			resampler,
			converted->data,
			frame_size,
			&src_ptr,
			in_samples);
		if (ret < 0)
			goto fail;

		src_offset += in_samples;

		if (ret == 0)
		{
			av_frame_unref(converted);
			continue;
		}

		converted->nb_samples = ret;
		converted->pts = ve->audio_next_pts;
		ve->audio_next_pts += ret;

		ret = avcodec_send_frame(ve->audio_ctx, converted);
		if (ret == AVERROR(EAGAIN))
		{
			ret = write_available_packets(ve, ve->audio_ctx, ve->audio_stream);
			if (ret < 0)
				goto fail;
			ret = avcodec_send_frame(ve->audio_ctx, converted);
		}
		if (ret < 0)
			goto fail;

		ret = write_available_packets(ve, ve->audio_ctx, ve->audio_stream);
		if (ret < 0)
			goto fail;

		av_frame_unref(converted);
	}

	/*
	 * Flush any samples swr_convert buffered internally so they are not lost
	 * before the silence padding runs.
	 */
	for (;;)
	{
		av_frame_unref(converted);
		converted->format = ve->audio_ctx->sample_fmt;
		converted->sample_rate = ve->audio_ctx->sample_rate;
		av_channel_layout_copy(&converted->ch_layout, &ve->audio_ctx->ch_layout);
		converted->nb_samples = frame_size;

		ret = av_frame_get_buffer(converted, 0);
		if (ret < 0)
			goto fail;

		ret = swr_convert(
			resampler,
			converted->data,
			frame_size,
			NULL,
			0);
		if (ret < 0)
			goto fail;
		if (ret == 0)
		{
			av_frame_unref(converted);
			break;
		}

		converted->nb_samples = ret;
		converted->pts = ve->audio_next_pts;
		ve->audio_next_pts += ret;

		ret = avcodec_send_frame(ve->audio_ctx, converted);
		if (ret == AVERROR(EAGAIN))
		{
			ret = write_available_packets(ve, ve->audio_ctx, ve->audio_stream);
			if (ret < 0)
				goto fail;
			ret = avcodec_send_frame(ve->audio_ctx, converted);
		}
		if (ret < 0)
			goto fail;

		ret = write_available_packets(ve, ve->audio_ctx, ve->audio_stream);
		if (ret < 0)
			goto fail;

		av_frame_unref(converted);
	}

	ret = 0;

fail:
	if (ret < 0)
	{
		renderingout_log_errorf(
			"RenderingOut: write_audio_from_pcm failed with error (%s)",
			av_err2str(ret));
	}
	av_frame_free(&converted);
	swr_free(&resampler);
	av_channel_layout_uninit(&src_layout);
	return ret;
}

/*
 * Writes silent audio frames until the audio timeline reaches the video
 * timeline. Used when no audio source was registered (or as padding after
 * a short source). Silent AAC packets are essentially free in size.
 */
static int write_silence_until_pts(VideoEncoder* ve, int64_t target_pts)
{
	int ret;

	while (ve->audio_next_pts < target_pts)
	{
		av_samples_set_silence(
			ve->audio_frame->data,
			0,
			ve->audio_frame->nb_samples,
			ve->audio_ctx->ch_layout.nb_channels,
			ve->audio_ctx->sample_fmt);

		ret = write_audio_frame(ve);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int finish_encoding(VideoEncoder* ve)
{
	int result = 0;
	int ret;

	ret = flush_encoder(ve);
	if (ret < 0)
		result = ret;

	if (ve->audio_enabled && ve->audio_ctx && result >= 0)
	{
		/*
		 * Video duration in encoder time_base is next_pts * time_base.
		 * Convert to audio time_base (1/sample_rate) for the silence target.
		 */
		int64_t video_duration_samples = av_rescale_q(
			ve->next_pts,
			ve->ctx->time_base,
			ve->audio_ctx->time_base);

		if (ve->pending_pcm_data)
		{
			ret = write_audio_from_pcm(
				ve,
				ve->pending_pcm_data,
				ve->pending_pcm_size,
				ve->pending_pcm_sample_rate,
				ve->pending_pcm_channels);
			if (ret < 0)
			{
				renderingout_log_errorf(
					"RenderingOut: audio mux failed (%s), padding silence",
					av_err2str(ret));
				ve->audio_next_pts = 0;
			}
		}

		ret = write_silence_until_pts(ve, video_duration_samples);
		if (ret < 0 && result >= 0)
			result = ret;

		ret = flush_audio_encoder(ve);
		if (ret < 0 && result >= 0)
			result = ret;
	}

	if (ve->header_written)
	{
		ret = av_write_trailer(ve->format);
		if (ret < 0 && result >= 0)
			result = ret;
		ve->header_written = 0;
	}

	if (ve->format && ve->format->pb &&
		!(ve->format->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_closep(&ve->format->pb);
		if (ret < 0 && result >= 0)
			result = ret;
	}

	renderingout_logf(
		"RenderingOut: finish_encoding completed for '%s'. Total video frames: %lld",
		ve->filename,
		(long long)ve->next_pts);

	avcodec_free_context(&ve->ctx);
	avcodec_free_context(&ve->audio_ctx);
	av_frame_free(&ve->audio_frame);
	av_freep(&ve->pending_pcm_data);
	ve->pending_pcm_size = 0;
	ve->pending_pcm_sample_rate = 0;
	ve->pending_pcm_channels = 0;
	avformat_free_context(ve->format);
	ve->format = NULL;
	ve->stream = NULL;
	ve->audio_stream = NULL;
	ve->audio_enabled = 0;
	ve->initialized = 0;
	return result;
}

static unsigned __stdcall encoder_worker_main(void* parameter)
{
	VideoEncoder* ve = (VideoEncoder*)parameter;
	HRESULT com_result;
	int com_initialized;
	int should_exit = 0;

	/*
	 * Unity's main thread is STA. Media Foundation requires MTA, so every
	 * encoder operation, including close, stays on this dedicated MTA thread.
	 */
	com_result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	com_initialized = SUCCEEDED(com_result);

	while (!should_exit)
	{
		EncoderWorkerCommand command;
		AVFrame* frame;
		enum AVPixelFormat texture_format;
		int multisampled;
		int result = 0;

		EnterCriticalSection(&ve->worker_lock);
		while (ve->worker_command == ENCODER_COMMAND_NONE)
			SleepConditionVariableCS(
				&ve->worker_changed, &ve->worker_lock, INFINITE);

		command = ve->worker_command;
		frame = ve->worker_frame;
		texture_format = ve->worker_texture_format;
		multisampled = ve->worker_multisampled;
		LeaveCriticalSection(&ve->worker_lock);

		switch (command)
		{
		case ENCODER_COMMAND_INITIALIZE:
			result = initialize_encoder(
				ve, texture_format, multisampled);
			if (result >= 0)
				result = initialize_output(ve);
			if (result < 0)
				ve->fatal_error = result;
			break;

		case ENCODER_COMMAND_ENCODE:
			result = send_frame_and_write_packets(ve, frame);
			av_frame_free(&frame);
			if (result < 0)
				ve->fatal_error = result;
			break;

		case ENCODER_COMMAND_FINISH:
			result = finish_encoding(ve);
			break;

		case ENCODER_COMMAND_EXIT:
			should_exit = 1;
			break;

		case ENCODER_COMMAND_NONE:
		default:
			result = AVERROR(EINVAL);
			break;
		}

		EnterCriticalSection(&ve->worker_lock);
		ve->worker_frame = NULL;
		ve->worker_result = result;
		ve->worker_command = ENCODER_COMMAND_NONE;
		WakeAllConditionVariable(&ve->worker_changed);
		LeaveCriticalSection(&ve->worker_lock);
	}

	if (com_initialized)
		CoUninitialize();
	return 0;
}

static int dispatch_worker_command(
	VideoEncoder* ve,
	EncoderWorkerCommand command,
	AVFrame* frame,
	enum AVPixelFormat texture_format,
	int multisampled)
{
	int result;

	if (!ve->worker)
		return AVERROR(EINVAL);

	EnterCriticalSection(&ve->worker_lock);
	while (ve->worker_command != ENCODER_COMMAND_NONE)
		SleepConditionVariableCS(
			&ve->worker_changed, &ve->worker_lock, INFINITE);

	ve->worker_frame = frame;
	ve->worker_texture_format = texture_format;
	ve->worker_multisampled = multisampled;
	ve->worker_command = command;
	WakeAllConditionVariable(&ve->worker_changed);

	while (ve->worker_command != ENCODER_COMMAND_NONE)
		SleepConditionVariableCS(
			&ve->worker_changed, &ve->worker_lock, INFINITE);

	result = ve->worker_result;
	LeaveCriticalSection(&ve->worker_lock);
	return result;
}

UNITY_INTERFACE_EXPORT VideoEncoder* UNITY_INTERFACE_API
video_encoder_create(
	int bit_rate,
	int width,
	int height,
	int fps,
	const char* filename)
{
	VideoEncoder* ve;
	int ret;

	if (bit_rate <= 0 || width <= 0 || height <= 0 || fps <= 0 ||
		!filename || (width & 1) || (height & 1))
		return NULL;
	if (!renderingout_get_hw_device())
		return NULL;

	ve = (VideoEncoder*)calloc(1, sizeof(*ve));
	if (!ve)
		return NULL;

	ve->bit_rate = bit_rate;
	ve->width = width;
	ve->height = height;
	ve->fps = fps;
	ve->filename = av_strdup(filename);
	if (!ve->filename)
		goto fail;

	renderingout_logf(
		"RenderingOut: video_encoder_create('%s', %dx%d @ %d fps, bitrate: %d bps)",
		filename, width, height, fps, bit_rate);

	ret = avformat_alloc_output_context2(
		&ve->format, NULL, NULL, filename);
	if (ret < 0 || !ve->format)
		goto fail;

	ve->packet = av_packet_alloc();
	if (!ve->packet)
		goto fail;

	ret = d3d11_frame_converter_create(
		&ve->converter,
		renderingout_get_hw_device(),
		width,
		height,
		fps);
	if (ret < 0)
		goto fail;

	InitializeCriticalSection(&ve->worker_lock);
	InitializeConditionVariable(&ve->worker_changed);
	ve->worker = (HANDLE)_beginthreadex(
		NULL, 0, encoder_worker_main, ve, 0, NULL);
	if (!ve->worker)
	{
		DeleteCriticalSection(&ve->worker_lock);
		goto fail;
	}

	return ve;

fail:
	d3d11_frame_converter_free(&ve->converter);
	av_packet_free(&ve->packet);
	avformat_free_context(ve->format);
	av_freep(&ve->filename);
	free(ve);
	return NULL;
}

UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API
video_encoder_submit_frame(VideoEncoder* ve, void* native_texture)
{
	ID3D11Resource* resource = (ID3D11Resource*)native_texture;
	D3D11_TEXTURE2D_DESC desc;
	enum AVPixelFormat texture_format;
	AVFrame* frame = NULL;
	int ret;

	if (!ve || !resource)
		return AVERROR(EINVAL);
	if (ve->fatal_error < 0)
		return ve->fatal_error;

	ret = d3d11_resource_texture_desc(resource, &desc);
	if (ret < 0)
		return ret;
	if ((int)desc.Width != ve->width || (int)desc.Height != ve->height)
		return AVERROR(EINVAL);

	texture_format = d3d11_texture_pixel_format(desc.Format);
	if (texture_format == AV_PIX_FMT_NONE)
		return AVERROR(ENOSYS);

	if (!ve->initialized)
	{
		ret = dispatch_worker_command(
			ve,
			ENCODER_COMMAND_INITIALIZE,
			NULL,
			texture_format,
			desc.SampleDesc.Count > 1);
		if (ret < 0)
		{
			renderingout_log_errorf(
				"RenderingOut: initialize encoder command failed (%s)",
				av_err2str(ret));
			return ret;
		}
	}
	else if (texture_format != ve->texture_format)
	{
		return AVERROR(EINVAL);
	}

	if (ve->direct_d3d11)
	{
		if (desc.SampleDesc.Count != 1)
			return AVERROR(EINVAL);
		ret = d3d11_frame_copy_to_hw(
			ve->converter,
			ve->hw_frames,
			resource,
			ve->next_pts,
			&frame);
	}
	else
	{
		ret = d3d11_frame_readback(
			ve->converter,
			resource,
			ve->input_format,
			ve->next_pts,
			&frame);
	}
	if (ret < 0)
	{
		renderingout_log_errorf(
			"RenderingOut: frame copy/readback failed (%s)",
			av_err2str(ret));
		return ret;
	}

	ret = dispatch_worker_command(
		ve,
		ENCODER_COMMAND_ENCODE,
		frame,
		AV_PIX_FMT_NONE,
		0);
	if (ret >= 0)
		++ve->next_pts;
	else
		renderingout_log_errorf(
			"RenderingOut: encode frame command failed (%s)",
			av_err2str(ret));
	return ret;
}

UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API
video_encoder_mux_audio(
	VideoEncoder* ve,
	const void* pcm_data,
	int pcm_length_bytes,
	int sample_rate,
	int channels)
{
	uint8_t* copy = NULL;

	if (!ve || !pcm_data || pcm_length_bytes <= 0 ||
		sample_rate <= 0 || channels <= 0 ||
		channels > AV_NUM_DATA_POINTERS ||
		pcm_length_bytes % (channels * (int)sizeof(float)) != 0)
		return AVERROR(EINVAL);
	if (ve->fatal_error < 0)
		return ve->fatal_error;
	if (!ve->audio_enabled)
		return AVERROR(EINVAL);

	copy = av_malloc(pcm_length_bytes);
	if (!copy)
		return AVERROR(ENOMEM);
	memcpy(copy, pcm_data, pcm_length_bytes);

	renderingout_logf(
		"RenderingOut: registered PCM audio data (%d bytes, %d Hz, %d channels, duration: %.2f sec)",
		pcm_length_bytes,
		sample_rate,
		channels,
		(double)pcm_length_bytes / (channels * sizeof(float) * sample_rate));

	/*
	 * Overwrites any previously registered buffer; the actual encode happens
	 * during finish_encoding so repeated calls naturally overwrite.
	 */
	av_freep(&ve->pending_pcm_data);
	ve->pending_pcm_data = copy;
	ve->pending_pcm_size = pcm_length_bytes;
	ve->pending_pcm_sample_rate = sample_rate;
	ve->pending_pcm_channels = channels;
	return 0;
}

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
video_encoder_free(VideoEncoder* ve)
{
	if (!ve)
		return;

	if (ve->worker)
	{
		dispatch_worker_command(
			ve, ENCODER_COMMAND_FINISH, NULL, AV_PIX_FMT_NONE, 0);
		dispatch_worker_command(
			ve, ENCODER_COMMAND_EXIT, NULL, AV_PIX_FMT_NONE, 0);
		WaitForSingleObject(ve->worker, INFINITE);
		CloseHandle(ve->worker);
		ve->worker = NULL;
		DeleteCriticalSection(&ve->worker_lock);
	}

	d3d11_frame_converter_free(&ve->converter);
	av_buffer_unref(&ve->hw_frames);
	av_packet_free(&ve->packet);
	avcodec_free_context(&ve->audio_ctx);
	av_frame_free(&ve->audio_frame);
	av_freep(&ve->pending_pcm_data);
	av_freep(&ve->filename);
	free(ve);
}
