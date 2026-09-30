#include "dlss/reactive.h"

#include <wrl/client.h>

#include <atomic>
#include <mutex>

#include "common/log.h"

using Microsoft::WRL::ComPtr;

namespace ds3::reactive {
namespace {

// What each context currently has bound. A context is only used by one thread
// at a time, so its slot needs no further locking.
struct ContextSlot {
  std::atomic<ID3D11DeviceContext*> ctx{nullptr};
  // Target 0 is the HDR scene and target 1 is bound too: DS3's forward and
  // transparent passes. (Deferred lighting writes the scene alone.)
  std::atomic<bool> scene{false};
  std::atomic<bool> blend{false};   // blending on target 0
  std::atomic<bool> writes{true};   // depth or stencil writes
};
constexpr size_t kMaxContexts = 64;
ContextSlot g_contexts[kMaxContexts];

std::atomic<ID3D11Resource*> g_scene{nullptr};
std::atomic<bool> g_active{false};
std::atomic<bool> g_ready{false};
std::atomic<uint32_t> g_redraws{0};

// Written on the immediate context's thread, read by any recording thread.
std::mutex g_mutex;
struct Resources {
  UINT w = 0, h = 0;
  ComPtr<ID3D11Texture2D> target;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11ShaderResourceView> srv;
} g_res;

ContextSlot* FindSlot(ID3D11DeviceContext* ctx, bool create) {
  for (ContextSlot& s : g_contexts) {
    ID3D11DeviceContext* owner = s.ctx.load(std::memory_order_acquire);
    if (owner == ctx) return &s;
    if (!owner) {
      if (!create) return nullptr;
      if (s.ctx.compare_exchange_strong(owner, ctx)) return &s;
      if (owner == ctx) return &s;
    }
  }
  return nullptr;
}

}  // namespace

void SetSceneColor(ID3D11Resource* scene) { g_scene.store(scene, std::memory_order_relaxed); }

void SetActive(bool on) { g_active.store(on, std::memory_order_relaxed); }

void OnSetRenderTargets(ID3D11DeviceContext* ctx, UINT count,
                        ID3D11RenderTargetView* const* rtvs) {
  bool scene = false;
  ID3D11Resource* want = g_scene.load(std::memory_order_relaxed);
  if (want && rtvs && count >= 2 && rtvs[0] && rtvs[1]) {
    ID3D11Resource* resource = nullptr;
    rtvs[0]->GetResource(&resource);
    scene = resource == want;
    if (resource) resource->Release();
  }
  ContextSlot* slot = FindSlot(ctx, true);
  if (slot) slot->scene.store(scene, std::memory_order_relaxed);
}

void OnSetBlendState(ID3D11DeviceContext* ctx, ID3D11BlendState* blend) {
  bool on = false;
  if (blend) {
    D3D11_BLEND_DESC d;
    blend->GetDesc(&d);
    on = d.RenderTarget[0].BlendEnable != FALSE;
  }
  ContextSlot* slot = FindSlot(ctx, true);
  if (slot) slot->blend.store(on, std::memory_order_relaxed);
}

void OnSetDepthStencilState(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* state) {
  bool writes = true;  // the default state writes depth
  if (state) {
    D3D11_DEPTH_STENCIL_DESC d;
    state->GetDesc(&d);
    writes = (d.DepthEnable && d.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO) ||
             (d.StencilEnable && d.StencilWriteMask != 0);
  }
  ContextSlot* slot = FindSlot(ctx, true);
  if (slot) slot->writes.store(writes, std::memory_order_relaxed);
}

void OnResetState(ID3D11DeviceContext* ctx) {
  ContextSlot* slot = FindSlot(ctx, false);
  if (!slot) return;
  slot->scene.store(false, std::memory_order_relaxed);
  slot->blend.store(false, std::memory_order_relaxed);
  slot->writes.store(true, std::memory_order_relaxed);
}

bool WantsRedraw(ID3D11DeviceContext* ctx) {
  if (!g_active.load(std::memory_order_relaxed) || !g_ready.load(std::memory_order_relaxed))
    return false;
  const ContextSlot* slot = FindSlot(ctx, false);
  return slot && slot->scene.load(std::memory_order_relaxed) &&
         slot->blend.load(std::memory_order_relaxed) &&
         !slot->writes.load(std::memory_order_relaxed);
}

bool Begin(ID3D11DeviceContext* ctx, SavedTargets* s) {
  ComPtr<ID3D11RenderTargetView> rtv;
  {
    std::lock_guard lock(g_mutex);
    rtv = g_res.rtv;
  }
  if (!rtv) return false;
  ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s->rtvs, &s->dsv);
  g_redraws.fetch_add(1, std::memory_order_relaxed);
  // Same depth buffer (hidden parts stay hidden); UAVs untouched.
  ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, rtv.GetAddressOf(), s->dsv,
                                                 D3D11_KEEP_UNORDERED_ACCESS_VIEWS, 0, nullptr,
                                                 nullptr);
  return true;
}

void End(ID3D11DeviceContext* ctx, SavedTargets* s) {
  ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s->rtvs,
                                                 s->dsv, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, 0,
                                                 nullptr, nullptr);
  for (auto* p : s->rtvs)
    if (p) p->Release();
  if (s->dsv) s->dsv->Release();
}

bool Prepare(ID3D11Device* device, ID3D11DeviceContext* ctx, UINT w, UINT h) {
  std::lock_guard lock(g_mutex);
  if (g_res.w == w && g_res.h == h && g_res.rtv) {
    g_ready = true;
    return true;
  }
  g_ready = false;
  g_res = Resources{};
  // Blendable float, like the HDR scene.
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  HRESULT hr = device->CreateTexture2D(&d, nullptr, &g_res.target);
  if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(g_res.target.Get(), nullptr, &g_res.rtv);
  if (SUCCEEDED(hr))
    hr = device->CreateShaderResourceView(g_res.target.Get(), nullptr, &g_res.srv);
  if (FAILED(hr)) {
    Log("reactive mask: creating the %ux%u target failed: 0x%08X", w, h, hr);
    g_res = Resources{};
    return false;
  }
  g_res.w = w;
  g_res.h = h;
  const FLOAT zero[4] = {};
  ctx->ClearRenderTargetView(g_res.rtv.Get(), zero);
  Log("reactive mask: target created for %ux%u", w, h);
  g_ready = true;
  return true;
}

ID3D11ShaderResourceView* Srv() {
  std::lock_guard lock(g_mutex);
  return g_ready ? g_res.srv.Get() : nullptr;
}

void Clear(ID3D11DeviceContext* ctx) {
  std::lock_guard lock(g_mutex);
  if (!g_res.rtv) return;
  const FLOAT zero[4] = {};
  ctx->ClearRenderTargetView(g_res.rtv.Get(), zero);
}

uint32_t TakeRedrawCount() { return g_redraws.exchange(0, std::memory_order_relaxed); }

}  // namespace ds3::reactive
