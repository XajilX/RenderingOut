#pragma once

#include <IUnityInterface.h>
#include <libavutil/buffer.h>

#ifdef _WIN32
#include <d3d11.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

int renderingout_is_supported(void);
AVBufferRef* renderingout_get_hw_device(void);

#ifdef _WIN32
ID3D11Device* renderingout_get_d3d11_device(void);
ID3D11DeviceContext* renderingout_get_d3d11_context(void);
#endif

void renderingout_logf(const char* fmt, ...);
void renderingout_log_errorf(const char* fmt, ...);
void renderingout_set_probe_logging_suppressed(int suppressed);

#ifdef __cplusplus
}
#endif
