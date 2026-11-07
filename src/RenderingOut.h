#pragma once

#include <IUnityInterface.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VideoEncoder VideoEncoder;

/*
 * quality: 0 = Low, 1 = Medium, 2 = High, 3 = Ultra.
 */
UNITY_INTERFACE_EXPORT VideoEncoder* UNITY_INTERFACE_API
video_encoder_create(
    int quality,
    int width,
    int height,
    int fps,
    const char* filename
);

/*
 * native_texture is the IntPtr returned by Texture.GetNativeTexturePtr().
 * On Windows it is an ID3D11Texture2D; on Linux it is a GLuint OpenGL texture
 * name. Must be called serially from the render thread that owns the texture
 * and has the corresponding graphics context current.
 * Returns 0 on success or a negative AVERROR value on failure.
 */
UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API
video_encoder_submit_frame(
    VideoEncoder* ve,
    void* native_texture
);

#ifndef _WIN32
/*
 * Queues a frame for execution from Unity's render thread. Call
 * GetRenderEventFunc() through GL.IssuePluginEvent after this function, then
 * wait with video_encoder_wait_frame().
 */
UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API
video_encoder_queue_frame(
    VideoEncoder* ve,
    void* native_texture
);

UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API
video_encoder_wait_frame(
    VideoEncoder* ve
);

#endif

/*
 * Registers a PCM buffer to be muxed into the output when video_encoder_free
 * is called. The PCM data is resampled and re-encoded to AAC.
 *
 * pcm_data:        pointer to interleaved normalized 32-bit float PCM samples.
 * pcm_length_bytes: number of bytes pointed to by pcm_data; must contain
 *                   complete interleaved sample frames.
 * sample_rate:     sampling rate in Hz (e.g. 44100).
 * channels:        number of audio channels (e.g. 2 for stereo).
 */
UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API
video_encoder_mux_audio(
    VideoEncoder* ve,
    const void* pcm_data,
    int pcm_length_bytes,
    int sample_rate,
    int channels
);

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
video_encoder_free(
    VideoEncoder* ve
);

#ifdef __cplusplus
}
#endif
