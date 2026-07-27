#include "Unity.h"

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>

#include <IUnityGraphicsD3D11.h>
#include <IUnityLog.h>

static ID3D11Device* s_device = NULL;
static ID3D11DeviceContext* s_device_context = NULL;
static AVBufferRef* s_hw_device = NULL;
static IUnityLog* s_unity_log = NULL;

ID3D11Device* renderingout_get_d3d11_device(void)
{
    return s_device;
}

ID3D11DeviceContext* renderingout_get_d3d11_context(void)
{
    return s_device_context;
}

AVBufferRef* renderingout_get_hw_device(void)
{
    return s_hw_device;
}

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
UnityPluginLoad(IUnityInterfaces* unity_interfaces)
{
    IUnityGraphicsD3D11* d3d11;
    AVHWDeviceContext* hwctx;
    AVD3D11VADeviceContext* d3dctx;
    int ret;

    s_unity_log = unity_interfaces->GetInterface(IUnityLog_GUID);
    UNITY_LOG(s_unity_log, "Rendering Out Plugin loading.");

    d3d11 = unity_interfaces->GetInterface(IUnityGraphicsD3D11_GUID);
    if (!d3d11)
    {
        UNITY_LOG_ERROR(s_unity_log, "D3D11 interface not available.");
        return;
    }

    /*
     * Unity owns this reference. FFmpeg receives its own AddRef below and will
     * release it when s_hw_device is unreferenced.
     */
    s_device = d3d11->GetDevice(d3d11);
    if (!s_device)
    {
        UNITY_LOG_ERROR(s_unity_log, "No D3D11 device.");
        return;
    }

    s_hw_device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!s_hw_device)
    {
        UNITY_LOG_ERROR(s_unity_log, "Failed to allocate the FFmpeg D3D11 device.");
        s_device = NULL;
        return;
    }

    hwctx = (AVHWDeviceContext*)s_hw_device->data;
    d3dctx = (AVD3D11VADeviceContext*)hwctx->hwctx;

    s_device->lpVtbl->AddRef(s_device);
    d3dctx->device = s_device;
    s_device->lpVtbl->GetImmediateContext(s_device, &d3dctx->device_context);

    ret = av_hwdevice_ctx_init(s_hw_device);
    if (ret < 0)
    {
        UNITY_LOG_ERROR(s_unity_log, "Failed to initialize the FFmpeg D3D11 device.");

        /*
         * av_hwdevice_ctx_init() did not complete, so release the COM
         * references installed above explicitly.
         */
        if (d3dctx->device_context)
        {
            d3dctx->device_context->lpVtbl->Release(d3dctx->device_context);
            d3dctx->device_context = NULL;
        }
        if (d3dctx->device)
        {
            d3dctx->device->lpVtbl->Release(d3dctx->device);
            d3dctx->device = NULL;
        }

        av_buffer_unref(&s_hw_device);
        s_device = NULL;
        return;
    }

    s_device_context = d3dctx->device_context;
    UNITY_LOG(s_unity_log, "Rendering Out Plugin loaded.");
}

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
UnityPluginUnload(void)
{
    /*
     * VideoEncoder instances must be freed before the plugin unloads because
     * their hardware frame pools retain this device context.
     */
    s_device_context = NULL;
    s_device = NULL;
    av_buffer_unref(&s_hw_device);
    s_unity_log = NULL;
}
