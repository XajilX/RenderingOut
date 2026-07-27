#pragma once

#include <d3d11.h>

#include <IUnityInterface.h>

#include <libavutil/buffer.h>

#ifdef __cplusplus
extern "C" {
#endif

ID3D11Device* renderingout_get_d3d11_device(void);
ID3D11DeviceContext* renderingout_get_d3d11_context(void);
AVBufferRef* renderingout_get_hw_device(void);

#ifdef __cplusplus
}
#endif
