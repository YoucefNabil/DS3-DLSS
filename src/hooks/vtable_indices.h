#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const size_t kSlotDevice_CreateVertexShader;
extern const size_t kSlotDevice_CreatePixelShader;
extern const size_t kSlotDevice_CreateComputeShader;
extern const size_t kSlotDevice_CreateDeferredContext;

extern const size_t kSlotCtx_Draw;
extern const size_t kSlotCtx_DrawIndexed;
extern const size_t kSlotCtx_DrawInstanced;
extern const size_t kSlotCtx_DrawIndexedInstanced;
extern const size_t kSlotCtx_Dispatch;
extern const size_t kSlotCtx_CopyResource;
extern const size_t kSlotCtx_CopySubresourceRegion;
extern const size_t kSlotCtx_ResolveSubresource;
extern const size_t kSlotCtx_ClearRenderTargetView;
extern const size_t kSlotCtx_ClearDepthStencilView;
extern const size_t kSlotCtx_ExecuteCommandList;
extern const size_t kSlotCtx_FinishCommandList;
extern const size_t kSlotCtx_Map;
extern const size_t kSlotCtx_Unmap;
extern const size_t kSlotCtx_UpdateSubresource;
extern const size_t kSlotCtx1_UpdateSubresource1;

extern const size_t kSlotFactory_CreateSwapChain;
extern const size_t kSlotFactory2_CreateSwapChainForHwnd;

extern const size_t kSlotSwapChain_Present;
extern const size_t kSlotSwapChain_ResizeBuffers;

#ifdef __cplusplus
}
#endif
