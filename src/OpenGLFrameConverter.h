#pragma once

#include <stdint.h>

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

typedef struct OpenGLFrameConverter OpenGLFrameConverter;

typedef struct OpenGLTextureInfo
{
    int width;
    int height;
    int multisampled;
    enum AVPixelFormat format;
} OpenGLTextureInfo;

/*
 * Queries an OpenGL texture supplied by Texture.GetNativeTexturePtr(). The
 * caller must invoke this on the render thread with Unity's OpenGL context
 * current.
 */
int opengl_texture_info(void* native_texture, OpenGLTextureInfo* info);

int opengl_frame_converter_create(
    OpenGLFrameConverter** converter,
    int width,
    int height,
    int fps
);

void opengl_frame_converter_free(OpenGLFrameConverter** converter);

/*
 * Reads the texture back from OpenGL and converts it to dst_format. OpenGL
 * stores the first texel row at the bottom of the image, so the readback is
 * flipped vertically to match video's top-down row order.
 */
int opengl_frame_capture(
    OpenGLFrameConverter* converter,
    void* native_texture,
    enum AVPixelFormat dst_format,
    int64_t pts,
    AVFrame** frame
);
