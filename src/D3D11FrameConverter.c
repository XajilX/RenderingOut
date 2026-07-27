#include "D3D11FrameConverter.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

struct D3D11FrameConverter
{
    AVBufferRef* hw_device;
    AVD3D11VADeviceContext* hw_context;
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11Texture2D* staging;
    ID3D11Texture2D* resolve;
    D3D11_TEXTURE2D_DESC staging_desc;
    D3D11_TEXTURE2D_DESC resolve_desc;
    struct SwsContext* sws;
    int width;
    int height;
};

static void lock_context(D3D11FrameConverter* converter)
{
    if (converter->hw_context->lock)
        converter->hw_context->lock(converter->hw_context->lock_ctx);
}

static void unlock_context(D3D11FrameConverter* converter)
{
    if (converter->hw_context->unlock)
        converter->hw_context->unlock(converter->hw_context->lock_ctx);
}

static DXGI_FORMAT normalized_dxgi_format(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return format;
    }
}

enum AVPixelFormat d3d11_texture_pixel_format(DXGI_FORMAT format)
{
    switch (normalized_dxgi_format(format))
    {
    case DXGI_FORMAT_NV12:
        return AV_PIX_FMT_NV12;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return AV_PIX_FMT_BGRA;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return AV_PIX_FMT_RGBA;
    default:
        return AV_PIX_FMT_NONE;
    }
}

int d3d11_resource_texture_desc(
    ID3D11Resource* resource,
    D3D11_TEXTURE2D_DESC* desc)
{
    D3D11_RESOURCE_DIMENSION dimension;

    if (!resource || !desc)
        return AVERROR(EINVAL);

    resource->lpVtbl->GetType(resource, &dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return AVERROR(EINVAL);

    /*
     * ID3D11Texture2D derives from ID3D11Resource. GetType above validates the
     * concrete interface before this internal cast; no ownership is acquired.
     */
    ((ID3D11Texture2D*)resource)->lpVtbl->GetDesc(
        (ID3D11Texture2D*)resource, desc);
    return 0;
}

static int validate_texture(
    const D3D11FrameConverter* converter,
    ID3D11Resource* resource,
    D3D11_TEXTURE2D_DESC* desc)
{
    int ret;

    if (!converter || !resource || !desc)
        return AVERROR(EINVAL);

    ret = d3d11_resource_texture_desc(resource, desc);
    if (ret < 0)
        return ret;
    if ((int)desc->Width != converter->width ||
        (int)desc->Height != converter->height)
        return AVERROR(EINVAL);
    if (d3d11_texture_pixel_format(desc->Format) == AV_PIX_FMT_NONE)
        return AVERROR(ENOSYS);

    return 0;
}

static int same_texture_shape(
    const D3D11_TEXTURE2D_DESC* left,
    const D3D11_TEXTURE2D_DESC* right)
{
    return left->Width == right->Width &&
           left->Height == right->Height &&
           left->Format == right->Format &&
           left->SampleDesc.Count == right->SampleDesc.Count &&
           left->SampleDesc.Quality == right->SampleDesc.Quality;
}

static int ensure_resolve_texture(
    D3D11FrameConverter* converter,
    const D3D11_TEXTURE2D_DESC* source)
{
    D3D11_TEXTURE2D_DESC desc = { 0 };
    HRESULT hr;

    desc.Width = source->Width;
    desc.Height = source->Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = normalized_dxgi_format(source->Format);
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;

    if (converter->resolve &&
        same_texture_shape(&converter->resolve_desc, &desc))
        return 0;

    if (converter->resolve)
    {
        converter->resolve->lpVtbl->Release(converter->resolve);
        converter->resolve = NULL;
    }

    hr = converter->device->lpVtbl->CreateTexture2D(
        converter->device, &desc, NULL, &converter->resolve);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;

    converter->resolve_desc = desc;
    return 0;
}

static int ensure_staging_texture(
    D3D11FrameConverter* converter,
    const D3D11_TEXTURE2D_DESC* source)
{
    D3D11_TEXTURE2D_DESC desc = { 0 };
    HRESULT hr;

    desc.Width = source->Width;
    desc.Height = source->Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = normalized_dxgi_format(source->Format);
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    if (converter->staging &&
        same_texture_shape(&converter->staging_desc, &desc))
        return 0;

    if (converter->staging)
    {
        converter->staging->lpVtbl->Release(converter->staging);
        converter->staging = NULL;
    }

    hr = converter->device->lpVtbl->CreateTexture2D(
        converter->device, &desc, NULL, &converter->staging);
    if (FAILED(hr))
        return AVERROR_EXTERNAL;

    converter->staging_desc = desc;
    return 0;
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

static void copy_rows(
    uint8_t* dst,
    int dst_stride,
    const uint8_t* src,
    int src_stride,
    int row_bytes,
    int rows)
{
    int y;
    for (y = 0; y < rows; ++y)
    {
        memcpy(dst, src, (size_t)row_bytes);
        dst += dst_stride;
        src += src_stride;
    }
}

static int copy_native_frame(
    AVFrame* frame,
    const D3D11_MAPPED_SUBRESOURCE* mapped,
    enum AVPixelFormat format,
    int width,
    int height)
{
    const uint8_t* source = (const uint8_t*)mapped->pData;

    switch (format)
    {
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_RGBA:
        copy_rows(
            frame->data[0], frame->linesize[0],
            source, (int)mapped->RowPitch,
            width * 4, height);
        return 0;

    case AV_PIX_FMT_NV12:
        copy_rows(
            frame->data[0], frame->linesize[0],
            source, (int)mapped->RowPitch,
            width, height);
        copy_rows(
            frame->data[1], frame->linesize[1],
            source + (size_t)mapped->RowPitch * height,
            (int)mapped->RowPitch,
            width, (height + 1) / 2);
        return 0;

    default:
        return AVERROR(ENOSYS);
    }
}

static int convert_mapped_frame(
    D3D11FrameConverter* converter,
    AVFrame* frame,
    const D3D11_MAPPED_SUBRESOURCE* mapped,
    enum AVPixelFormat source_format,
    enum AVPixelFormat destination_format)
{
    const uint8_t* source_data[4] = { 0 };
    int source_stride[4] = { 0 };
    const int* coefficients;
    int colorspace;
    int scaled;

    source_data[0] = (const uint8_t*)mapped->pData;
    source_stride[0] = (int)mapped->RowPitch;
    if (source_format == AV_PIX_FMT_NV12)
    {
        source_data[1] = source_data[0] +
                         (size_t)mapped->RowPitch * converter->height;
        source_stride[1] = (int)mapped->RowPitch;
    }

    converter->sws = sws_getCachedContext(
        converter->sws,
        converter->width,
        converter->height,
        source_format,
        converter->width,
        converter->height,
        destination_format,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL);
    if (!converter->sws)
        return AVERROR(ENOMEM);

    colorspace = converter->height >= 720 ? SWS_CS_ITU709 : SWS_CS_SMPTE170M;
    coefficients = sws_getCoefficients(colorspace);
    if (source_format == AV_PIX_FMT_BGRA || source_format == AV_PIX_FMT_RGBA)
    {
        int ret = sws_setColorspaceDetails(
            converter->sws,
            coefficients,
            1,
            coefficients,
            0,
            0,
            1 << 16,
            1 << 16);
        if (ret < 0)
            return ret;
    }

    scaled = sws_scale(
        converter->sws,
        source_data,
        source_stride,
        0,
        converter->height,
        frame->data,
        frame->linesize);
    return scaled == converter->height ? 0 : AVERROR_EXTERNAL;
}

int d3d11_frame_converter_create(
    D3D11FrameConverter** converter,
    AVBufferRef* hw_device,
    int width,
    int height)
{
    D3D11FrameConverter* result;
    AVHWDeviceContext* device_context;
    AVD3D11VADeviceContext* d3d11_context;

    if (!converter || !hw_device || width <= 0 || height <= 0)
        return AVERROR(EINVAL);

    device_context = (AVHWDeviceContext*)hw_device->data;
    if (!device_context || device_context->type != AV_HWDEVICE_TYPE_D3D11VA)
        return AVERROR(EINVAL);

    d3d11_context = (AVD3D11VADeviceContext*)device_context->hwctx;
    if (!d3d11_context || !d3d11_context->device ||
        !d3d11_context->device_context)
        return AVERROR(EINVAL);

    result = (D3D11FrameConverter*)calloc(1, sizeof(*result));
    if (!result)
        return AVERROR(ENOMEM);

    result->hw_device = av_buffer_ref(hw_device);
    if (!result->hw_device)
    {
        free(result);
        return AVERROR(ENOMEM);
    }

    result->hw_context = d3d11_context;
    result->device = d3d11_context->device;
    result->context = d3d11_context->device_context;
    result->width = width;
    result->height = height;
    *converter = result;
    return 0;
}

void d3d11_frame_converter_free(D3D11FrameConverter** converter)
{
    D3D11FrameConverter* value;

    if (!converter || !*converter)
        return;

    value = *converter;
    sws_freeContext(value->sws);
    if (value->staging)
        value->staging->lpVtbl->Release(value->staging);
    if (value->resolve)
        value->resolve->lpVtbl->Release(value->resolve);
    av_buffer_unref(&value->hw_device);
    free(value);
    *converter = NULL;
}

int d3d11_frame_copy_to_hw(
    D3D11FrameConverter* converter,
    AVBufferRef* hw_frames,
    ID3D11Resource* resource,
    int64_t pts,
    AVFrame** frame)
{
    D3D11_TEXTURE2D_DESC source_desc;
    AVFrame* result;
    UINT destination_subresource;
    int ret;

    if (!frame || !hw_frames)
        return AVERROR(EINVAL);
    *frame = NULL;

    ret = validate_texture(converter, resource, &source_desc);
    if (ret < 0)
        return ret;
    if (source_desc.SampleDesc.Count != 1)
        return AVERROR(EINVAL);

    result = av_frame_alloc();
    if (!result)
        return AVERROR(ENOMEM);

    ret = av_hwframe_get_buffer(hw_frames, result, 0);
    if (ret < 0)
    {
        av_frame_free(&result);
        return ret;
    }

    destination_subresource = (UINT)(uintptr_t)result->data[1];
    lock_context(converter);
    converter->context->lpVtbl->CopySubresourceRegion(
        converter->context,
        (ID3D11Resource*)result->data[0],
        destination_subresource,
        0,
        0,
        0,
        resource,
        0,
        NULL);
    unlock_context(converter);

    set_frame_metadata(result, converter->width, converter->height, pts);
    *frame = result;
    return 0;
}

int d3d11_frame_readback(
    D3D11FrameConverter* converter,
    ID3D11Resource* resource,
    enum AVPixelFormat dst_format,
    int64_t pts,
    AVFrame** frame)
{
    D3D11_TEXTURE2D_DESC source_desc;
    D3D11_MAPPED_SUBRESOURCE mapped = { 0 };
    ID3D11Resource* copy_source;
    enum AVPixelFormat source_format;
    AVFrame* result = NULL;
    HRESULT hr;
    int mapped_ok = 0;
    int ret;

    if (!frame)
        return AVERROR(EINVAL);
    *frame = NULL;

    ret = validate_texture(converter, resource, &source_desc);
    if (ret < 0)
        return ret;
    source_format = d3d11_texture_pixel_format(source_desc.Format);

    copy_source = resource;
    if (source_desc.SampleDesc.Count > 1)
    {
        if (source_format == AV_PIX_FMT_NV12)
            return AVERROR(ENOSYS);

        ret = ensure_resolve_texture(converter, &source_desc);
        if (ret < 0)
            return ret;
        copy_source = (ID3D11Resource*)converter->resolve;
    }

    ret = ensure_staging_texture(converter, &source_desc);
    if (ret < 0)
        return ret;

    lock_context(converter);
    if (source_desc.SampleDesc.Count > 1)
    {
        converter->context->lpVtbl->ResolveSubresource(
            converter->context,
            (ID3D11Resource*)converter->resolve,
            0,
            resource,
            0,
            normalized_dxgi_format(source_desc.Format));
    }
    converter->context->lpVtbl->CopyResource(
        converter->context,
        (ID3D11Resource*)converter->staging,
        copy_source);

    hr = converter->context->lpVtbl->Map(
        converter->context,
        (ID3D11Resource*)converter->staging,
        0,
        D3D11_MAP_READ,
        0,
        &mapped);
    if (FAILED(hr))
    {
        unlock_context(converter);
        return AVERROR_EXTERNAL;
    }
    mapped_ok = 1;

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

    if (source_format == dst_format)
        ret = copy_native_frame(
            result, &mapped, source_format,
            converter->width, converter->height);
    else
        ret = convert_mapped_frame(
            converter, result, &mapped, source_format, dst_format);

    if (ret < 0)
        goto done;

    set_frame_metadata(result, converter->width, converter->height, pts);
    *frame = result;
    result = NULL;

done:
    if (mapped_ok)
        converter->context->lpVtbl->Unmap(
            converter->context,
            (ID3D11Resource*)converter->staging,
            0);
    unlock_context(converter);
    av_frame_free(&result);
    return ret;
}
