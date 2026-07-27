#include "RenderingOut.h"

struct VideoEncoder
{
	AVCodecContext* ctx;
	FILE* file;
};


UNITY_INTERFACE_EXPORT VideoEncoder* UNITY_INTERFACE_API
video_encoder_create(
	const int bit_rate,
	const int width,
	const int height,
	const int fps,

	const char* filename)
{
	const char* candidates[] =
	{
#ifdef _WIN32
	"h264_nvenc",
	"h264_amf",
	"h264_qsv",
#endif

#ifdef __APPLE__
	"h264_videotoolbox",
#endif

#ifdef __linux__
	"h264_vaapi",
#endif

	"libx264"
	};

	AVCodecContext* ctx = NULL;
	FILE* file;

	//// find & open encoder ////
	const int candidate_count = sizeof(candidates) / sizeof(candidates[0]);
	for (int i = 0; i < candidate_count; i++)
	{
		const AVCodec* codec = avcodec_find_encoder_by_name(candidates[i]);
		if (!codec)
			continue;

		ctx = avcodec_alloc_context3(codec);
		if (!ctx)
			continue;

		ctx->bit_rate = bit_rate;
		ctx->width = width;
		ctx->height = height;
		ctx->framerate = (AVRational){ fps, 1 };

		ctx->time_base = (AVRational){ 1, 90000 };
		ctx->gop_size = fps;
		ctx->max_b_frames = 0;
		ctx->pix_fmt = AV_PIX_FMT_YUV420P;

		if (codec->id == AV_CODEC_ID_H264)
		{
			av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
		}

		if (avcodec_open2(ctx, codec, NULL) >= 0)
		{
			break;
		}

		avcodec_free_context(&ctx);
	}

	if (!ctx)
	{
		return NULL;
	}

	//// open file ////
	file = fopen(filename, "wb");
	if (!file)
	{
		avcodec_free_context(&ctx);
		return NULL;
	}

	VideoEncoder* ve = (VideoEncoder*)malloc(sizeof(VideoEncoder));
	if (!ve)
	{
		avcodec_free_context(&ctx);
		fclose(file);
		return NULL;
	}
	ve->ctx = ctx;
	ve->file = file;
	return ve;
}

//TODO...

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
video_encoder_free(VideoEncoder* ve)
{
	avcodec_free_context(&ve->ctx);
	fclose(ve->file);
	free(ve);
}