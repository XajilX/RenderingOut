#pragma once

#include <d3d11.h>

#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

typedef struct D3D11FrameConverter D3D11FrameConverter;

enum AVPixelFormat d3d11_texture_pixel_format(DXGI_FORMAT format);

int d3d11_resource_texture_desc(
    ID3D11Resource* resource,
    D3D11_TEXTURE2D_DESC* desc
);

int d3d11_frame_converter_create(
    D3D11FrameConverter** converter,
    AVBufferRef* hw_device,
    int width,
    int height,
    int fps
);

void d3d11_frame_converter_free(D3D11FrameConverter** converter);

/*
 * Verifies that a non-NV12 source can be converted to an encoder-ready NV12
 * surface entirely on the GPU. Call this before selecting h264_mf direct mode.
 */
int d3d11_frame_prepare_nv12_conversion(
    D3D11FrameConverter* converter,
    DXGI_FORMAT source_format
);

/*
 * Copies resource into a surface owned by FFmpeg's hardware frame pool. When
 * the destination is NV12 and the source is RGB, D3D11 video processing is
 * used for GPU-side color conversion. This keeps Unity free to reuse its
 * render target while the encoder owns the resulting AVFrame.
 */
int d3d11_frame_copy_to_hw(
    D3D11FrameConverter* converter,
    AVBufferRef* hw_frames,
    ID3D11Resource* resource,
    int64_t pts,
    AVFrame** frame
);

/*
 * Reads resource back to a software AVFrame. If its native pixel format
 * already equals dst_format this is only a row copy.
 */
int d3d11_frame_readback(
    D3D11FrameConverter* converter,
    ID3D11Resource* resource,
    enum AVPixelFormat dst_format,
    int64_t pts,
    AVFrame** frame
);
