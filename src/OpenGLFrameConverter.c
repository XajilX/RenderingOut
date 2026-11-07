#include "OpenGLFrameConverter.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>

#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>

#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif

#ifndef GL_PIXEL_PACK_BUFFER_BINDING
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88EF
#endif

struct OpenGLFrameConverter
{
    struct SwsContext* sws;
    int width;
    int height;
};

static int valid_color_format(const OpenGLTextureInfo* info)
{
    return info->width > 0 && info->height > 0 &&
           info->format == AV_PIX_FMT_RGBA;
}

int opengl_texture_info(void* native_texture, OpenGLTextureInfo* info)
{
    GLint previous_active_texture = GL_TEXTURE0;
    GLint previous_texture = 0;
    GLint width = 0;
    GLint height = 0;
    GLint red_bits = 0;
    GLint green_bits = 0;
    GLint blue_bits = 0;
    GLuint texture;

    if (!native_texture || !info)
        return AVERROR(EINVAL);

    memset(info, 0, sizeof(*info));
    texture = (GLuint)(uintptr_t)native_texture;
    if (texture == 0)
        return AVERROR(EINVAL);

    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_RED_SIZE, &red_bits);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_GREEN_SIZE, &green_bits);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_BLUE_SIZE, &blue_bits);

    glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);
    glActiveTexture((GLenum)previous_active_texture);

    if (width <= 0 || height <= 0)
        return AVERROR(EINVAL);

    /*
     * Unity render textures are normally 8-bit RGBA. Requesting GL_RGBA from
     * glGetTexImage also handles RGB textures by supplying opaque alpha, but
     * floating-point and compressed color layouts need a different pipeline.
     */
    if (red_bits != 8 || green_bits != 8 || blue_bits != 8)
        return AVERROR(ENOSYS);

    info->width = (int)width;
    info->height = (int)height;
    info->multisampled = 0;
    info->format = AV_PIX_FMT_RGBA;
    return 0;
}

int opengl_frame_converter_create(
    OpenGLFrameConverter** converter,
    int width,
    int height,
    int fps)
{
    OpenGLFrameConverter* result;

    if (!converter || width <= 0 || height <= 0 || fps <= 0)
        return AVERROR(EINVAL);
    *converter = NULL;

    result = (OpenGLFrameConverter*)calloc(1, sizeof(*result));
    if (!result)
        return AVERROR(ENOMEM);

    result->width = width;
    result->height = height;
    *converter = result;
    return 0;
}

void opengl_frame_converter_free(OpenGLFrameConverter** converter)
{
    OpenGLFrameConverter* value;

    if (!converter || !*converter)
        return;

    value = *converter;
    sws_freeContext(value->sws);
    free(value);
    *converter = NULL;
}

static void set_frame_metadata(AVFrame* frame, int width, int height, int64_t pts)
{
    frame->width = width;
    frame->height = height;
    frame->pts = pts;
    frame->duration = 1;

    if (height >= 720)
    {
        frame->colorspace = AVCOL_SPC_BT709;
        frame->color_primaries = AVCOL_PRI_BT709;
        frame->color_trc = AVCOL_TRC_BT709;
    }
    else
    {
        frame->colorspace = AVCOL_SPC_SMPTE170M;
        frame->color_primaries = AVCOL_PRI_SMPTE170M;
        frame->color_trc = AVCOL_TRC_SMPTE170M;
    }
    frame->color_range = AVCOL_RANGE_MPEG;
}

static void flip_rows(
    uint8_t* destination,
    const uint8_t* source,
    int row_bytes,
    int height)
{
    int y;

    for (y = 0; y < height; ++y)
    {
        memcpy(
            destination + (size_t)y * row_bytes,
            source + (size_t)(height - 1 - y) * row_bytes,
            (size_t)row_bytes);
    }
}

int opengl_frame_capture(
    OpenGLFrameConverter* converter,
    void* native_texture,
    enum AVPixelFormat dst_format,
    int64_t pts,
    AVFrame** frame)
{
    GLint previous_active_texture = GL_TEXTURE0;
    GLint previous_texture = 0;
    GLint previous_alignment = 4;
    GLint previous_pack_buffer = 0;
    OpenGLTextureInfo info;
    const uint8_t* source_data[4] = { 0 };
    int source_stride[4] = { 0 };
    const int* coefficients;
    int colorspace;
    uint8_t* pixels = NULL;
    uint8_t* flipped = NULL;
    AVFrame* result = NULL;
    GLuint texture;
    int row_bytes;
    int scaled;
    int ret;

    if (!converter || !native_texture || !frame)
        return AVERROR(EINVAL);
    *frame = NULL;

    ret = opengl_texture_info(native_texture, &info);
    if (ret < 0)
        return ret;
    if (!valid_color_format(&info) ||
        info.width != converter->width ||
        info.height != converter->height)
        return AVERROR(EINVAL);

    row_bytes = converter->width * 4;
    pixels = (uint8_t*)av_malloc((size_t)row_bytes * converter->height);
    flipped = (uint8_t*)av_malloc((size_t)row_bytes * converter->height);
    if (!pixels || !flipped)
    {
        ret = AVERROR(ENOMEM);
        goto done;
    }

    texture = (GLuint)(uintptr_t)native_texture;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previous_alignment);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous_pack_buffer);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_PACK_ALIGNMENT, previous_alignment);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)previous_pack_buffer);
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);
    glActiveTexture((GLenum)previous_active_texture);

    flip_rows(flipped, pixels, row_bytes, converter->height);

    result = av_frame_alloc();
    if (!result)
    {
        ret = AVERROR(ENOMEM);
        goto done;
    }

    result->format = dst_format;
    result->width = converter->width;
    result->height = converter->height;
    ret = av_frame_get_buffer(result, 32);
    if (ret < 0)
        goto done;

    converter->sws = sws_getCachedContext(
        converter->sws,
        converter->width,
        converter->height,
        AV_PIX_FMT_RGBA,
        converter->width,
        converter->height,
        dst_format,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL);
    if (!converter->sws)
    {
        ret = AVERROR(ENOMEM);
        goto done;
    }

    source_data[0] = flipped;
    source_stride[0] = row_bytes;
    colorspace = converter->height >= 720 ? SWS_CS_ITU709 : SWS_CS_SMPTE170M;
    coefficients = sws_getCoefficients(colorspace);
    ret = sws_setColorspaceDetails(
        converter->sws,
        coefficients,
        1,
        coefficients,
        0,
        0,
        1 << 16,
        1 << 16);
    if (ret < 0)
        goto done;

    scaled = sws_scale(
        converter->sws,
        source_data,
        source_stride,
        0,
        converter->height,
        result->data,
        result->linesize);
    if (scaled != converter->height)
    {
        ret = AVERROR_EXTERNAL;
        goto done;
    }

    set_frame_metadata(result, converter->width, converter->height, pts);
    *frame = result;
    result = NULL;
    ret = 0;

done:
    av_frame_free(&result);
    av_freep(&flipped);
    av_freep(&pixels);
    return ret;
}
