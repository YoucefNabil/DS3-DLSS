/* Vtable slot indices, computed from the SDK's C-style COM declarations so we
 * never hand-count slots. Compiled as C on purpose: the *Vtbl structs only
 * exist in C (or CINTERFACE) mode. */
#define CINTERFACE
#define COBJMACROS
#include <stddef.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include "hooks/vtable_indices.h"

#define SLOT(vtbl, method) (offsetof(vtbl, method) / sizeof(void*))

const size_t kSlotDevice_CreateVertexShader = SLOT(ID3D11DeviceVtbl, CreateVertexShader);
const size_t kSlotDevice_CreatePixelShader = SLOT(ID3D11DeviceVtbl, CreatePixelShader);
const size_t kSlotDevice_CreateComputeShader = SLOT(ID3D11DeviceVtbl, CreateComputeShader);
const size_t kSlotDevice_CreateDeferredContext = SLOT(ID3D11DeviceVtbl, CreateDeferredContext);

const size_t kSlotCtx_Draw = SLOT(ID3D11DeviceContextVtbl, Draw);
const size_t kSlotCtx_DrawIndexed = SLOT(ID3D11DeviceContextVtbl, DrawIndexed);
const size_t kSlotCtx_DrawInstanced = SLOT(ID3D11DeviceContextVtbl, DrawInstanced);
const size_t kSlotCtx_DrawIndexedInstanced = SLOT(ID3D11DeviceContextVtbl, DrawIndexedInstanced);
const size_t kSlotCtx_Dispatch = SLOT(ID3D11DeviceContextVtbl, Dispatch);
const size_t kSlotCtx_CopyResource = SLOT(ID3D11DeviceContextVtbl, CopyResource);
const size_t kSlotCtx_CopySubresourceRegion = SLOT(ID3D11DeviceContextVtbl, CopySubresourceRegion);
const size_t kSlotCtx_ResolveSubresource = SLOT(ID3D11DeviceContextVtbl, ResolveSubresource);
const size_t kSlotCtx_ClearRenderTargetView = SLOT(ID3D11DeviceContextVtbl, ClearRenderTargetView);
const size_t kSlotCtx_ClearDepthStencilView = SLOT(ID3D11DeviceContextVtbl, ClearDepthStencilView);
const size_t kSlotCtx_ExecuteCommandList = SLOT(ID3D11DeviceContextVtbl, ExecuteCommandList);
const size_t kSlotCtx_FinishCommandList = SLOT(ID3D11DeviceContextVtbl, FinishCommandList);
const size_t kSlotCtx_Map = SLOT(ID3D11DeviceContextVtbl, Map);
const size_t kSlotCtx_Unmap = SLOT(ID3D11DeviceContextVtbl, Unmap);
const size_t kSlotCtx_UpdateSubresource = SLOT(ID3D11DeviceContextVtbl, UpdateSubresource);
const size_t kSlotCtx1_UpdateSubresource1 = SLOT(ID3D11DeviceContext1Vtbl, UpdateSubresource1);

const size_t kSlotFactory_CreateSwapChain = SLOT(IDXGIFactoryVtbl, CreateSwapChain);
const size_t kSlotFactory2_CreateSwapChainForHwnd = SLOT(IDXGIFactory2Vtbl, CreateSwapChainForHwnd);

const size_t kSlotSwapChain_Present = SLOT(IDXGISwapChainVtbl, Present);
const size_t kSlotSwapChain_ResizeBuffers = SLOT(IDXGISwapChainVtbl, ResizeBuffers);
