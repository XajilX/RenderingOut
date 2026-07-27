#include<Unity.h>

static ID3D11Device* s_device = NULL;
static AVBufferRef* s_hw_device = NULL;
static AVBufferRef* s_hw_frames = NULL;

static IUnityLog* s_UnityLog = NULL;

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
UnityPluginLoad(IUnityInterfaces* unityInterfaces)
{
	s_UnityLog = unityInterfaces->GetInterface(IUnityLog_GUID);

	UNITY_LOG(s_UnityLog, "Rendering Out Plugin loading.");

	IUnityGraphicsD3D11* d3d11 = unityInterfaces->GetInterface(IUnityGraphicsD3D11_GUID);
	if (!d3d11)
	{
		UNITY_LOG_ERROR(s_UnityLog, "D3D11 interface not available.");
		return;
	}

	s_device = d3d11->GetDevice(d3d11);
	if (!s_device)
	{
		UNITY_LOG_ERROR(s_UnityLog, "No D3D11 device.");
		return;
	}

	s_hw_device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
	if (!s_hw_device)
	{
		UNITY_LOG_ERROR(s_UnityLog, "Failed to av_hwdevice_ctx_alloc. availables:");
		enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
		while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
		{
			UNITY_LOG(s_UnityLog, av_hwdevice_get_type_name(type));
		}
		s_device->lpVtbl->Release(s_device);
		s_device = NULL;
		return;
	}

	AVHWDeviceContext* hwctx = (AVHWDeviceContext*)s_hw_device->data;
	AVD3D11VADeviceContext* d3dctx = (AVD3D11VADeviceContext*)hwctx->hwctx;
	s_device->lpVtbl->AddRef(s_device);
	d3dctx->device = s_device;
	s_device->lpVtbl->GetImmediateContext(
		s_device,
		&d3dctx->device_context
	);
	int ret = av_hwdevice_ctx_init(s_hw_device);
	if (ret < 0)
	{
		UNITY_LOG_ERROR(s_UnityLog, "Failed to av_hwdevice_ctx_init.");
		/* init 失败时 av_buffer_unref 不会触发 d3d11va_device_free，
		   必须手动释放 AddRef 的 device 和 GetImmediateContext 的 context */
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
		return;
	}

	UNITY_LOG(s_UnityLog, "Rendering Out Plugin loaded.");
}

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
UnityPluginUnload()
{
	if (s_hw_frames)
		av_buffer_unref(&s_hw_frames);
	if (s_hw_device)
		av_buffer_unref(&s_hw_device);
	if (s_device)
	{
		s_device->lpVtbl->Release(s_device);
		s_device = NULL;
	}

	s_UnityLog = NULL;
}