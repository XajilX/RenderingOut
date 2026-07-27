#pragma once

#include <IUnityInterface.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VideoEncoder VideoEncoder;

UNITY_INTERFACE_EXPORT VideoEncoder* UNITY_INTERFACE_API
video_encoder_create(
    int bit_rate,
    int width,
    int height,
    int fps,
    const char* filename
);

/*
 * native_texture is the IntPtr returned by Texture.GetNativeTexturePtr().
 * Must be called serially from the render thread that owns the texture.
 * Returns 0 on success or a negative AVERROR value on failure.
 */
UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API
video_encoder_submit_frame(
    VideoEncoder* ve,
    void* native_texture
);

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
video_encoder_free(
    VideoEncoder* ve
);

#ifdef __cplusplus
}
#endif
