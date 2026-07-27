#include "RenderingOut.h"

#include "D3D11FrameConverter.h"
#include "Unity.h"

#include <errno.h>
#include <objbase.h>
#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>

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
    AVFormatContext* format;
    AVStream* stream;
    AVPacket* packet;
    AVBufferRef* hw_frames;
    D3D11FrameConverter* converter;
    char* filename;

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
};

static int write_available_packets(VideoEncoder* ve)
{
    int ret;

    if (!ve->header_written || !ve->stream)
        return AVERROR(EINVAL);

    for (;;)
    {
        ret = avcodec_receive_packet(ve->ctx, ve->packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;

        av_packet_rescale_ts(
            ve->packet,
            ve->ctx->time_base,
            ve->stream->time_base);
        ve->packet->stream_index = ve->stream->index;

        ret = av_interleaved_write_frame(ve->format, ve->packet);
        av_packet_unref(ve->packet);
        if (ret < 0)
            return ret;
    }
}

static int create_hw_frames(
    VideoEncoder* ve,
    enum AVPixelFormat software_format,
    AVBufferRef** frames)
{
    AVBufferRef* result;
    AVHWFramesContext* context;
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
        av_dict_set(options, "hw_encoding", "1", 0);
    }
    else if (strcmp(candidate->name, "h264_nvenc") == 0)
    {
        av_dict_set(options, "delay", "0", 0);
        av_dict_set(options, "zerolatency", "1", 0);
    }
}

static int try_open_encoder(
    VideoEncoder* ve,
    const EncoderCandidate* candidate)
{
    const AVCodec* codec;
    AVCodecContext* context = NULL;
    AVBufferRef* frames = NULL;
    AVDictionary* options = NULL;
    int ret;

    codec = avcodec_find_encoder_by_name(candidate->name);
    if (!codec)
        return AVERROR_ENCODER_NOT_FOUND;

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
        ret = create_hw_frames(ve, candidate->input_format, &frames);
        if (ret < 0)
            goto fail;

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
        goto fail;

    ve->ctx = context;
    ve->hw_frames = frames;
    ve->input_format = candidate->input_format;
    ve->direct_d3d11 = candidate->direct_d3d11;
    av_log(
        NULL,
        AV_LOG_INFO,
        "RenderingOut: selected %s (%s, input=%s)\n",
        candidate->name,
        candidate->direct_d3d11 ? "D3D11" : "readback",
        av_get_pix_fmt_name(candidate->input_format));
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
    int candidate_count = 0;
    int i;
    int last_error = AVERROR_ENCODER_NOT_FOUND;

    if (!multisampled)
    {
        candidates[candidate_count++] =
            (EncoderCandidate){ "h264_nvenc", texture_format, 1 };
        if (texture_format == AV_PIX_FMT_NV12)
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
        last_error = try_open_encoder(ve, &candidates[i]);
        if (last_error >= 0)
        {
            ve->initialized = 1;
            ve->texture_format = texture_format;
            return 0;
        }
    }

    return last_error;
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

    if (!(ve->format->oformat->flags & AVFMT_NOFILE))
    {
        ret = avio_open(&ve->format->pb, ve->filename, AVIO_FLAG_WRITE);
        if (ret < 0)
            return ret;
    }

    if (strcmp(ve->format->oformat->name, "mp4") == 0 ||
        strcmp(ve->format->oformat->name, "mov") == 0)
        av_dict_set(&options, "movflags", "+faststart", 0);

    ret = avformat_write_header(ve->format, &options);
    av_dict_free(&options);
    if (ret < 0)
        return ret;

    ve->header_written = 1;
    return 0;
}

static int send_frame_and_write_packets(VideoEncoder* ve, AVFrame* frame)
{
    int ret;

    ret = avcodec_send_frame(ve->ctx, frame);
    if (ret == AVERROR(EAGAIN))
    {
        ret = write_available_packets(ve);
        if (ret < 0)
            return ret;
        ret = avcodec_send_frame(ve->ctx, frame);
    }
    if (ret < 0)
        return ret;

    return write_available_packets(ve);
}

static int flush_encoder(VideoEncoder* ve)
{
    int ret;

    if (!ve->initialized || !ve->header_written)
        return 0;

    ret = avcodec_send_frame(ve->ctx, NULL);
    if (ret == AVERROR(EAGAIN))
    {
        ret = write_available_packets(ve);
        if (ret < 0)
            return ret;
        ret = avcodec_send_frame(ve->ctx, NULL);
    }
    if (ret < 0 && ret != AVERROR_EOF)
        return ret;

    return write_available_packets(ve);
}

static int finish_encoding(VideoEncoder* ve)
{
    int result = 0;
    int ret;

    ret = flush_encoder(ve);
    if (ret < 0)
        result = ret;

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

    avcodec_free_context(&ve->ctx);
    avformat_free_context(ve->format);
    ve->format = NULL;
    ve->stream = NULL;
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
        height);
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
            return ret;
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
        return ret;

    ret = dispatch_worker_command(
        ve,
        ENCODER_COMMAND_ENCODE,
        frame,
        AV_PIX_FMT_NONE,
        0);
    if (ret >= 0)
        ++ve->next_pts;
    return ret;
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
    av_freep(&ve->filename);
    free(ve);
}
