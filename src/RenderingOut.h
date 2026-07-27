// RenderingOut.h: 标准系统包含文件的包含文件
// 或项目特定的包含文件。

#pragma once

#include <stdio.h>
#include <stdlib.h>

// TODO: 在此处引用程序需要的其他标头。

#include <libavcodec/avcodec.h>

#include <libavutil/opt.h>
#include <libavutil/imgutils.h>

#include <IUnityInterface.h>

/* candidates 数量由 sizeof 自动推算，不再硬编码 */

typedef struct VideoEncoder VideoEncoder;

UNITY_INTERFACE_EXPORT VideoEncoder* UNITY_INTERFACE_API
video_encoder_create(
	const int bit_rate,
	const int width,
	const int height,
	const int fps,

	const char* filename
);

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
video_encoder_free(
	VideoEncoder* ve
);