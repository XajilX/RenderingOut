#include "Unity.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <libavutil/hwcontext.h>
#include <libavutil/log.h>

#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>

#include <IUnityGraphicsD3D11.h>
#else
#include <IUnityGraphics.h>
#endif
#include <IUnityLog.h>

#ifdef _WIN32
static ID3D11Device* s_device = NULL;
static ID3D11DeviceContext* s_device_context = NULL;
#else
static int s_supported = 0;
#endif
static AVBufferRef* s_hw_device = NULL;
static IUnityLog* s_unity_log = NULL;

#if defined(_MSC_VER)
#define RENDERINGOUT_THREAD_LOCAL __declspec(thread)
#else
#define RENDERINGOUT_THREAD_LOCAL _Thread_local
#endif

static RENDERINGOUT_THREAD_LOCAL int s_probe_logging_suppressed = 0;

static void renderingout_ffmpeg_log_callback(
	void* context,
	int level,
	const char* format,
	va_list args)
{
	char buffer[2048];
	size_t length;
	int print_prefix = 1;

        if (!s_unity_log || s_probe_logging_suppressed ||
                level > av_log_get_level())
                return;

	av_log_format_line2(
		context,
		level,
		format,
		args,
		buffer,
		sizeof(buffer),
		&print_prefix);

	length = strlen(buffer);
	while (length > 0 &&
		(buffer[length - 1] == '\n' || buffer[length - 1] == '\r'))
		buffer[--length] = '\0';
	if (length == 0)
		return;

	if (level <= AV_LOG_ERROR)
		UNITY_LOG_ERROR(s_unity_log, buffer);
	else
		UNITY_LOG(s_unity_log, buffer);
}

void renderingout_logf(const char* fmt, ...)
{
        if (!s_unity_log || s_probe_logging_suppressed)
                return;
	char buffer[2048];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	UNITY_LOG(s_unity_log, buffer);
}

void renderingout_log_errorf(const char* fmt, ...)
{
        if (!s_unity_log || s_probe_logging_suppressed)
                return;
	char buffer[2048];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
        UNITY_LOG_ERROR(s_unity_log, buffer);
}

void renderingout_set_probe_logging_suppressed(int suppressed)
{
        s_probe_logging_suppressed = suppressed != 0;
}

AVBufferRef* renderingout_get_hw_device(void)
{
	return s_hw_device;
}

int renderingout_is_supported(void)
{
#ifdef _WIN32
	return s_hw_device != NULL;
#else
	return s_supported;
#endif
}

#ifdef _WIN32
ID3D11Device* renderingout_get_d3d11_device(void)
{
	return s_device;
}

ID3D11DeviceContext* renderingout_get_d3d11_context(void)
{
	return s_device_context;
}
#endif

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
UnityPluginLoad(IUnityInterfaces* unity_interfaces)
{
#ifdef _WIN32
	IUnityGraphicsD3D11* d3d11;
	AVHWDeviceContext* hwctx;
	AVD3D11VADeviceContext* d3dctx;
	int ret;
#else
	IUnityGraphics* graphics;
	UnityGfxRenderer renderer;
#endif

	if (!unity_interfaces)
		return;

	/*
	 * UnityInterfaceGUID is a non-trivial type in C++, so the C++ ABI for
	 * GetInterface(UnityInterfaceGUID) differs from the C ABI. C plugins must
	 * use GetInterfaceSplit, as UNITY_GET_INTERFACE does in the Unity headers.
	 */
	s_unity_log = (IUnityLog*)unity_interfaces->GetInterfaceSplit(
		IUnityLog_GUID.m_GUIDHigh,
		IUnityLog_GUID.m_GUIDLow);
	if (!s_unity_log)
		return;

	av_log_set_level(AV_LOG_VERBOSE);
	av_log_set_callback(renderingout_ffmpeg_log_callback);
	UNITY_LOG(s_unity_log, "RenderingOut: plugin loading.");

#ifdef _WIN32
	d3d11 = (IUnityGraphicsD3D11*)unity_interfaces->GetInterfaceSplit(
		IUnityGraphicsD3D11_GUID.m_GUIDHigh,
		IUnityGraphicsD3D11_GUID.m_GUIDLow);
	if (!d3d11)
	{
		UNITY_LOG_ERROR(s_unity_log, "RenderingOut: D3D11 interface not available.");
		return;
	}

	/*
	 * Unity owns this reference. FFmpeg receives its own AddRef below and will
	 * release it when s_hw_device is unreferenced.
	 */
	s_device = d3d11->GetDevice(d3d11);
	if (!s_device)
	{
		UNITY_LOG_ERROR(s_unity_log, "RenderingOut: no D3D11 device.");
		return;
	}

	s_hw_device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
	if (!s_hw_device)
	{
		UNITY_LOG_ERROR(s_unity_log, "RenderingOut: failed to allocate the FFmpeg D3D11 device.");
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
		UNITY_LOG_ERROR(s_unity_log, "RenderingOut: failed to initialize the FFmpeg D3D11 device.");

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
	UNITY_LOG(s_unity_log, "RenderingOut: plugin loaded successfully.");
#else
	graphics = (IUnityGraphics*)unity_interfaces->GetInterfaceSplit(
		IUnityGraphics_GUID.m_GUIDHigh,
		IUnityGraphics_GUID.m_GUIDLow);
	if (!graphics)
	{
		UNITY_LOG_ERROR(
			s_unity_log,
			"RenderingOut: Unity graphics interface not available.");
		return;
	}

	renderer = graphics->GetRenderer();
	if (renderer != kUnityGfxRendererOpenGLCore)
	{
		UNITY_LOG_ERROR(
			s_unity_log,
			"RenderingOut: Linux requires Unity's OpenGL Core renderer. "
			"Launch Unity with -force-glcore (Vulkan is not supported).");
		return;
	}

	s_supported = 1;
	UNITY_LOG(
		s_unity_log,
		"RenderingOut: plugin loaded successfully with OpenGL Core.");
#endif
}

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
UnityPluginUnload(void)
{
	/*
	 * VideoEncoder instances must be freed before the plugin unloads because
	 * they may retain renderer resources and an active worker thread.
	 */
#ifdef _WIN32
	s_device_context = NULL;
	s_device = NULL;
	av_buffer_unref(&s_hw_device);
#else
	s_supported = 0;
#endif
	av_log_set_callback(av_log_default_callback);
	s_unity_log = NULL;
}
