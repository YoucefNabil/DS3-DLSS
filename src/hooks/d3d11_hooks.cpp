// D3D11/DXGI API interception. Hooks patch the shared code inside d3d11.dll /
// dxgi.dll (via MinHook on vtable targets); the game executable is not touched.
#include "hooks/d3d11_hooks.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <mutex>

#include "common/config.h"
#include "camera/scene_camera.h"
#include "common/log.h"
#include "dlss/dlaa.h"
#include "hooks/frame_dump.h"
#include "hooks/shader_registry.h"
#include "hooks/vtable_indices.h"

namespace ds3::hooks {
namespace {

std::once_flag g_device_hooks_once;
std::once_flag g_deferred_hooks_once;
std::once_flag g_swap_chain_hooks_once;

// Returns false (and logs) when the slot's code is already hooked, e.g. when a
// deferred context shares its implementation with the immediate context.
template <typename Fn>
bool HookSlot(void* object, size_t slot, Fn detour, Fn* original, const char* name) {
  void* target = (*reinterpret_cast<void***>(object))[slot];
  MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(detour),
                                   reinterpret_cast<void**>(original));
  // Enabling is queued and applied in one batch (ApplyQueued): each apply
  // suspends every thread in the process, so doing it per hook stalls startup.
  if (status == MH_OK) status = MH_QueueEnableHook(target);
  Log("hook %-36s slot %2zu at %p: %s", name, slot, target, MH_StatusToString(status));
  return status == MH_OK;
}

void ApplyQueued() {
  const MH_STATUS status = MH_ApplyQueued();
  Log("hooks enabled: %s", MH_StatusToString(status));
}

// ---- ID3D11Device --------------------------------------------------------

using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
                                                         ID3D11ClassLinkage*, ID3D11VertexShader**);
using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
                                                        ID3D11ClassLinkage*, ID3D11PixelShader**);
using CreateComputeShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
                                                          ID3D11ClassLinkage*, ID3D11ComputeShader**);
using CreateDeferredContextFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT,
                                                            ID3D11DeviceContext**);
CreateVertexShaderFn o_CreateVertexShader;
CreatePixelShaderFn o_CreatePixelShader;
CreateComputeShaderFn o_CreateComputeShader;
CreateDeferredContextFn o_CreateDeferredContext;

HRESULT STDMETHODCALLTYPE Hk_CreateVertexShader(ID3D11Device* self, const void* code, SIZE_T len,
                                                ID3D11ClassLinkage* linkage, ID3D11VertexShader** out) {
  const HRESULT hr = o_CreateVertexShader(self, code, len, linkage, out);
  if (SUCCEEDED(hr) && out) shaders::Register(*out, code, len, "vs");
  return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreatePixelShader(ID3D11Device* self, const void* code, SIZE_T len,
                                               ID3D11ClassLinkage* linkage, ID3D11PixelShader** out) {
  const HRESULT hr = o_CreatePixelShader(self, code, len, linkage, out);
  if (SUCCEEDED(hr) && out) shaders::Register(*out, code, len, "ps");
  return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateComputeShader(ID3D11Device* self, const void* code, SIZE_T len,
                                                 ID3D11ClassLinkage* linkage,
                                                 ID3D11ComputeShader** out) {
  const HRESULT hr = o_CreateComputeShader(self, code, len, linkage, out);
  if (SUCCEEDED(hr) && out) shaders::Register(*out, code, len, "cs");
  return hr;
}

// ---- ID3D11DeviceContext -------------------------------------------------
// Instantiated once for the immediate context (0) and once for deferred
// contexts (1), in case d3d11.dll implements them with different code.

using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT,
                                                        INT, UINT);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*,
                                                ID3D11Resource*);
using CopySubresourceRegionFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*,
                                                         UINT, UINT, UINT, UINT, ID3D11Resource*,
                                                         UINT, const D3D11_BOX*);
using ResolveSubresourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                                      ID3D11Resource*, UINT, DXGI_FORMAT);
using ClearRenderTargetViewFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                                         ID3D11RenderTargetView*, const FLOAT[4]);
using ClearDepthStencilViewFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                                         ID3D11DepthStencilView*, UINT, FLOAT,
                                                         UINT8);
using ExecuteCommandListFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*,
                                                      BOOL);
using FinishCommandListFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL,
                                                        ID3D11CommandList**);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP,
                                          UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdateSubresourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                                     const D3D11_BOX*, const void*, UINT, UINT);
using UpdateSubresource1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*, UINT,
                                                      const D3D11_BOX*, const void*, UINT, UINT,
                                                      UINT);

template <int Instance>
struct ContextHooks {
  static inline DrawFn o_Draw;
  static inline DrawIndexedFn o_DrawIndexed;
  static inline DrawInstancedFn o_DrawInstanced;
  static inline DrawIndexedInstancedFn o_DrawIndexedInstanced;
  static inline DispatchFn o_Dispatch;
  static inline CopyResourceFn o_CopyResource;
  static inline CopySubresourceRegionFn o_CopySubresourceRegion;
  static inline ResolveSubresourceFn o_ResolveSubresource;
  static inline ClearRenderTargetViewFn o_ClearRenderTargetView;
  static inline ClearDepthStencilViewFn o_ClearDepthStencilView;
  static inline ExecuteCommandListFn o_ExecuteCommandList;
  static inline FinishCommandListFn o_FinishCommandList;
  static inline MapFn o_Map;
  static inline UnmapFn o_Unmap;
  static inline UpdateSubresourceFn o_UpdateSubresource;
  static inline UpdateSubresource1Fn o_UpdateSubresource1;

  static void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* self, UINT vertices, UINT start) {
    dlaa::BeforeDraw(self);
    if (frame_dump::Active()) frame_dump::RecordDraw(self, "Draw", vertices, 1);
    o_Draw(self, vertices, start);
  }

  static void STDMETHODCALLTYPE DrawIndexed(ID3D11DeviceContext* self, UINT indices, UINT start,
                                            INT base) {
    if (frame_dump::Active()) frame_dump::RecordDraw(self, "DrawIndexed", indices, 1);
    o_DrawIndexed(self, indices, start, base);
  }

  static void STDMETHODCALLTYPE DrawInstanced(ID3D11DeviceContext* self, UINT vertices,
                                              UINT instances, UINT start, UINT start_instance) {
    if (frame_dump::Active()) frame_dump::RecordDraw(self, "DrawInstanced", vertices, instances);
    o_DrawInstanced(self, vertices, instances, start, start_instance);
  }

  static void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D11DeviceContext* self, UINT indices,
                                                     UINT instances, UINT start, INT base,
                                                     UINT start_instance) {
    if (frame_dump::Active())
      frame_dump::RecordDraw(self, "DrawIndexedInstanced", indices, instances);
    o_DrawIndexedInstanced(self, indices, instances, start, base, start_instance);
  }

  static void STDMETHODCALLTYPE Dispatch(ID3D11DeviceContext* self, UINT x, UINT y, UINT z) {
    if (frame_dump::Active()) frame_dump::RecordDispatch(self, x, y, z);
    o_Dispatch(self, x, y, z);
  }

  static void STDMETHODCALLTYPE CopyResource(ID3D11DeviceContext* self, ID3D11Resource* dst,
                                             ID3D11Resource* src) {
    if (frame_dump::Active()) frame_dump::RecordCopy(self, "CopyResource", dst, src);
    o_CopyResource(self, dst, src);
  }

  static void STDMETHODCALLTYPE CopySubresourceRegion(ID3D11DeviceContext* self,
                                                      ID3D11Resource* dst, UINT dst_sub, UINT x,
                                                      UINT y, UINT z, ID3D11Resource* src,
                                                      UINT src_sub, const D3D11_BOX* box) {
    if (frame_dump::Active()) frame_dump::RecordCopy(self, "CopySubresourceRegion", dst, src);
    o_CopySubresourceRegion(self, dst, dst_sub, x, y, z, src, src_sub, box);
  }

  static void STDMETHODCALLTYPE ResolveSubresource(ID3D11DeviceContext* self, ID3D11Resource* dst,
                                                   UINT dst_sub, ID3D11Resource* src,
                                                   UINT src_sub, DXGI_FORMAT format) {
    if (frame_dump::Active()) frame_dump::RecordCopy(self, "ResolveSubresource", dst, src);
    o_ResolveSubresource(self, dst, dst_sub, src, src_sub, format);
  }

  static void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11DeviceContext* self,
                                                      ID3D11RenderTargetView* rtv,
                                                      const FLOAT color[4]) {
    if (frame_dump::Active()) frame_dump::RecordClear(self, "ClearRTV", rtv);
    o_ClearRenderTargetView(self, rtv, color);
  }

  static void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DeviceContext* self,
                                                      ID3D11DepthStencilView* dsv, UINT flags,
                                                      FLOAT depth, UINT8 stencil) {
    dlaa::OnClearDepth(dsv);
    if (frame_dump::Active()) frame_dump::RecordClear(self, "ClearDSV", dsv);
    o_ClearDepthStencilView(self, dsv, flags, depth, stencil);
  }

  static void STDMETHODCALLTYPE ExecuteCommandList(ID3D11DeviceContext* self,
                                                   ID3D11CommandList* list, BOOL restore) {
    if (frame_dump::Active()) frame_dump::RecordExecuteCommandList(self, list);
    o_ExecuteCommandList(self, list, restore);
  }

  static HRESULT STDMETHODCALLTYPE FinishCommandList(ID3D11DeviceContext* self, BOOL restore,
                                                     ID3D11CommandList** list) {
    const HRESULT hr = o_FinishCommandList(self, restore, list);
    if (SUCCEEDED(hr) && list && *list && frame_dump::Active())
      frame_dump::RecordFinishCommandList(self, *list);
    return hr;
  }

  static HRESULT STDMETHODCALLTYPE Map(ID3D11DeviceContext* self, ID3D11Resource* r, UINT sub,
                                       D3D11_MAP type, UINT flags,
                                       D3D11_MAPPED_SUBRESOURCE* mapped) {
    const HRESULT hr = o_Map(self, r, sub, type, flags, mapped);
    if (SUCCEEDED(hr) && mapped && frame_dump::Active())
      frame_dump::RecordMap(self, r, type, mapped->pData);
    return hr;
  }

  static void STDMETHODCALLTYPE Unmap(ID3D11DeviceContext* self, ID3D11Resource* r, UINT sub) {
    if (frame_dump::Active()) frame_dump::RecordUnmap(self, r);
    o_Unmap(self, r, sub);
  }

  static void STDMETHODCALLTYPE UpdateSubresource(ID3D11DeviceContext* self, ID3D11Resource* r,
                                                  UINT sub, const D3D11_BOX* box,
                                                  const void* data, UINT row, UINT depth) {
    alignas(16) uint8_t scratch[camera::kSceneCbBytes];
    const void* sent = camera::OnConstantUpload(r, box, data, scratch);
    if (frame_dump::Active()) frame_dump::RecordUpdate(self, "UpdateSubresource", r, box, sent);
    o_UpdateSubresource(self, r, sub, box, sent, row, depth);
  }

  static void STDMETHODCALLTYPE UpdateSubresource1(ID3D11DeviceContext1* self, ID3D11Resource* r,
                                                   UINT sub, const D3D11_BOX* box,
                                                   const void* data, UINT row, UINT depth,
                                                   UINT flags) {
    alignas(16) uint8_t scratch[camera::kSceneCbBytes];
    const void* sent = camera::OnConstantUpload(r, box, data, scratch);
    if (frame_dump::Active()) frame_dump::RecordUpdate(self, "UpdateSubresource1", r, box, sent);
    o_UpdateSubresource1(self, r, sub, box, sent, row, depth, flags);
  }

  static void Install(ID3D11DeviceContext* ctx, const char* kind) {
    char name[64];
    auto hook = [&](size_t slot, auto detour, auto* original, const char* method) {
      std::snprintf(name, sizeof(name), "%s::%s", kind, method);
      HookSlot(ctx, slot, detour, original, name);
    };
    hook(kSlotCtx_Draw, &Draw, &o_Draw, "Draw");
    hook(kSlotCtx_DrawIndexed, &DrawIndexed, &o_DrawIndexed, "DrawIndexed");
    hook(kSlotCtx_DrawInstanced, &DrawInstanced, &o_DrawInstanced, "DrawInstanced");
    hook(kSlotCtx_DrawIndexedInstanced, &DrawIndexedInstanced, &o_DrawIndexedInstanced,
         "DrawIndexedInstanced");
    hook(kSlotCtx_Dispatch, &Dispatch, &o_Dispatch, "Dispatch");
    hook(kSlotCtx_CopyResource, &CopyResource, &o_CopyResource, "CopyResource");
    hook(kSlotCtx_CopySubresourceRegion, &CopySubresourceRegion, &o_CopySubresourceRegion,
         "CopySubresourceRegion");
    hook(kSlotCtx_ResolveSubresource, &ResolveSubresource, &o_ResolveSubresource,
         "ResolveSubresource");
    hook(kSlotCtx_ClearRenderTargetView, &ClearRenderTargetView, &o_ClearRenderTargetView,
         "ClearRenderTargetView");
    hook(kSlotCtx_ClearDepthStencilView, &ClearDepthStencilView, &o_ClearDepthStencilView,
         "ClearDepthStencilView");
    hook(kSlotCtx_ExecuteCommandList, &ExecuteCommandList, &o_ExecuteCommandList,
         "ExecuteCommandList");
    hook(kSlotCtx_FinishCommandList, &FinishCommandList, &o_FinishCommandList,
         "FinishCommandList");
    hook(kSlotCtx_Map, &Map, &o_Map, "Map");
    hook(kSlotCtx_Unmap, &Unmap, &o_Unmap, "Unmap");
    hook(kSlotCtx_UpdateSubresource, &UpdateSubresource, &o_UpdateSubresource,
         "UpdateSubresource");
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) {
      std::snprintf(name, sizeof(name), "%s::UpdateSubresource1", kind);
      HookSlot(ctx1, kSlotCtx1_UpdateSubresource1, &UpdateSubresource1, &o_UpdateSubresource1,
               name);
      ctx1->Release();
    }
  }
};

HRESULT STDMETHODCALLTYPE Hk_CreateDeferredContext(ID3D11Device* self, UINT flags,
                                                   ID3D11DeviceContext** out) {
  const HRESULT hr = o_CreateDeferredContext(self, flags, out);
  Log("CreateDeferredContext -> 0x%08X (%p)", hr, out ? static_cast<void*>(*out) : nullptr);
  if (SUCCEEDED(hr) && out && *out) {
    // Slots whose code is shared with the immediate context report
    // "already created" and stay covered by the immediate-context hooks.
    std::call_once(g_deferred_hooks_once, [ctx = *out] {
      ContextHooks<1>::Install(ctx, "Deferred");
      ApplyQueued();
    });
  }
  return hr;
}

// ---- IDXGISwapChain ------------------------------------------------------

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT,
                                                    UINT);
PresentFn o_Present;
ResizeBuffersFn o_ResizeBuffers;

bool KeyPressedOnce(int vk) {
  static bool was_down[256] = {};
  if (vk <= 0 || vk > 255) return false;
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool pressed = down && !was_down[vk];
  was_down[vk] = down;
  return pressed;
}

bool GameHasFocus() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
}

HRESULT STDMETHODCALLTYPE Hk_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
  static std::atomic<bool> logged_first{false};
  if (!logged_first.exchange(true)) {
    DXGI_SWAP_CHAIN_DESC desc = {};
    self->GetDesc(&desc);
    Log("first Present: %ux%u fmt=%d buffers=%u windowed=%d swap_effect=%d",
        desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format, desc.BufferCount,
        desc.Windowed, desc.SwapEffect);
  }
  static uint64_t presents = 0;
  ++presents;
  if (GameHasFocus() && KeyPressedOnce(GetConfig().dump_frame_key)) frame_dump::Request();
  if (GetConfig().dump_frame_at > 0 && presents == static_cast<uint64_t>(GetConfig().dump_frame_at))
    frame_dump::Request();
  if (GameHasFocus() && KeyPressedOnce(GetConfig().dlss_toggle_key)) dlaa::Toggle();
  frame_dump::OnPresent();
  camera::OnPresent();
  dlaa::OnPresent();
  return o_Present(self, sync, flags);
}

HRESULT STDMETHODCALLTYPE Hk_ResizeBuffers(IDXGISwapChain* self, UINT count, UINT width,
                                           UINT height, DXGI_FORMAT format, UINT flags) {
  const HRESULT hr = o_ResizeBuffers(self, count, width, height, format, flags);
  Log("ResizeBuffers(%u, %ux%u, fmt=%d) -> 0x%08X", count, width, height, format, hr);
  return hr;
}

// ---- IDXGIFactory --------------------------------------------------------

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*,
                                                      DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
CreateSwapChainFn o_CreateSwapChain;
CreateSwapChainForHwndFn o_CreateSwapChainForHwnd;

HRESULT STDMETHODCALLTYPE Hk_CreateSwapChain(IDXGIFactory* self, IUnknown* device,
                                             DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
  const HRESULT hr = o_CreateSwapChain(self, device, desc, out);
  Log("IDXGIFactory::CreateSwapChain -> 0x%08X", hr);
  if (SUCCEEDED(hr) && out && *out) OnSwapChainCreated(*out);
  return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device,
                                                    HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                    IDXGIOutput* output, IDXGISwapChain1** out) {
  const HRESULT hr = o_CreateSwapChainForHwnd(self, device, hwnd, desc, fs, output, out);
  Log("IDXGIFactory2::CreateSwapChainForHwnd -> 0x%08X", hr);
  if (SUCCEEDED(hr) && out && *out) OnSwapChainCreated(*out);
  return hr;
}

void HookFactoryOf(ID3D11Device* device) {
  IDXGIDevice* dxgi_device = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIFactory* factory = nullptr;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
      SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
      SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
    HookSlot(factory, kSlotFactory_CreateSwapChain, &Hk_CreateSwapChain, &o_CreateSwapChain,
             "IDXGIFactory::CreateSwapChain");
    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2)))) {
      HookSlot(factory2, kSlotFactory2_CreateSwapChainForHwnd, &Hk_CreateSwapChainForHwnd,
               &o_CreateSwapChainForHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
      factory2->Release();
    }
  } else {
    Log("warning: could not reach the DXGI factory from the device");
  }
  if (factory) factory->Release();
  if (adapter) adapter->Release();
  if (dxgi_device) dxgi_device->Release();
}

}  // namespace

void OnDeviceCreated(ID3D11Device* device) {
  std::call_once(g_device_hooks_once, [device] {
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
      Log("MH_Initialize failed: %s", MH_StatusToString(init));
      return;
    }
    Log("device %p feature level 0x%X: installing hooks", static_cast<void*>(device),
        device->GetFeatureLevel());

    HookSlot(device, kSlotDevice_CreateVertexShader, &Hk_CreateVertexShader,
             &o_CreateVertexShader, "Device::CreateVertexShader");
    HookSlot(device, kSlotDevice_CreatePixelShader, &Hk_CreatePixelShader, &o_CreatePixelShader,
             "Device::CreatePixelShader");
    HookSlot(device, kSlotDevice_CreateComputeShader, &Hk_CreateComputeShader,
             &o_CreateComputeShader, "Device::CreateComputeShader");
    HookSlot(device, kSlotDevice_CreateDeferredContext, &Hk_CreateDeferredContext,
             &o_CreateDeferredContext, "Device::CreateDeferredContext");

    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    ContextHooks<0>::Install(ctx, "Immediate");
    ctx->Release();

    HookFactoryOf(device);
    ApplyQueued();
  });
}

void OnSwapChainCreated(IDXGISwapChain* swap_chain) {
  std::call_once(g_swap_chain_hooks_once, [swap_chain] {
    HookSlot(swap_chain, kSlotSwapChain_Present, &Hk_Present, &o_Present, "SwapChain::Present");
    HookSlot(swap_chain, kSlotSwapChain_ResizeBuffers, &Hk_ResizeBuffers, &o_ResizeBuffers,
             "SwapChain::ResizeBuffers");
    ApplyQueued();
  });
}

}  // namespace ds3::hooks
