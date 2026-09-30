#pragma once
// Reactive mask for DLSS ("bias current colour"). Where it is set, DLSS trusts
// the current frame over its history. Transparent effects (spell and bonfire
// glows, particles, fire) have no depth or motion vectors of their own, so
// DLSS smears or flickers them. Every blended draw into the HDR scene is
// repeated once into a black float target with the game's own shaders and
// blend state: the result is what the effects added to each pixel, compared
// with the final colour later. Only the render targets are swapped, and they
// are restored right after.

#include <d3d11.h>

#include <cstdint>

namespace ds3::reactive {

// DLAA side, immediate context only.
void SetSceneColor(ID3D11Resource* scene);  // identity only; the texture DLAA runs on
void SetActive(bool on);
bool Prepare(ID3D11Device* device, ID3D11DeviceContext* ctx, UINT w, UINT h);
ID3D11ShaderResourceView* Srv();       // null until prepared
void Clear(ID3D11DeviceContext* ctx);  // after use, ready for the next frame
uint32_t TakeRedrawCount();            // draws repeated since the last call

// Context hooks, any thread.
void OnSetRenderTargets(ID3D11DeviceContext* ctx, UINT count,
                        ID3D11RenderTargetView* const* rtvs);
void OnSetBlendState(ID3D11DeviceContext* ctx, ID3D11BlendState* blend);
void OnSetDepthStencilState(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* state);
void OnResetState(ID3D11DeviceContext* ctx);  // the context's state went back to defaults
bool WantsRedraw(ID3D11DeviceContext* ctx);   // cheap; false for almost every draw

// Rebinds our target, runs `draw` (the original draw call), restores.
template <typename Draw>
void Redraw(ID3D11DeviceContext* ctx, Draw draw);

// ---- implementation detail -----------------------------------------------
struct SavedTargets {
  ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  ID3D11DepthStencilView* dsv = nullptr;
};
bool Begin(ID3D11DeviceContext* ctx, SavedTargets* saved);
void End(ID3D11DeviceContext* ctx, SavedTargets* saved);

template <typename Draw>
void Redraw(ID3D11DeviceContext* ctx, Draw draw) {
  SavedTargets saved;
  if (!Begin(ctx, &saved)) return;
  draw();
  End(ctx, &saved);
}

}  // namespace ds3::reactive
