#pragma once
// Per-object motion vectors. The game draws moving objects (characters,
// enemies, animated props) into a half-res 8-bit velocity buffer for its motion
// blur, using vertex shaders that output each vertex's current and previous
// clip position (object and bone motion included, never jittered). Those draws
// are repeated at full resolution with our own pixel shader into a float
// target, which the motion-vector pass then prefers over camera-only motion.
// The repeat's viewport is shifted by the frame's jitter, so object pixels line
// up exactly with the jittered scene.
// Only D3D11 state is changed, and it is restored right after each repeat.

#include <d3d11.h>

#include <cstddef>
#include <cstdint>

namespace ds3::object_motion {

// The velocity pass only runs when the in-game Motion Blur option is on. The
// blur itself can then be hidden by skipping its draws (HideMotionBlur).

// Device hooks: remember the game's velocity vertex shaders and blur pixel shader.
void OnVertexShaderCreated(ID3D11VertexShader* vs, const void* code, size_t length, uint64_t hash);
void OnPixelShaderCreated(ID3D11PixelShader* ps, uint64_t hash);

// Context hooks, any thread.
void OnSetVertexShader(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs);
void OnSetPixelShader(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps);
bool WantsRedraw(ID3D11DeviceContext* ctx);  // cheap; true for velocity draws while active
bool SkipDraw(ID3D11DeviceContext* ctx);     // cheap; true for motion blur draws while hidden

// Rebinds our target and pixel shader, runs `draw` (the original draw call), restores.
template <typename Draw>
void Redraw(ID3D11DeviceContext* ctx, Draw draw);

// DLAA side, immediate context only.
void SetActive(bool active, bool hide_motion_blur);
// Creates/refreshes the target for w x h and the view of the scene depth.
bool Prepare(ID3D11Device* device, ID3D11DeviceContext* ctx, UINT w, UINT h,
             ID3D11Texture2D* scene_depth, float proj_z);
ID3D11ShaderResourceView* Srv();       // null until prepared
void Clear(ID3D11DeviceContext* ctx);  // after use, ready for the next frame
uint32_t TakeRedrawCount();            // draws repeated since the last call

// ---- implementation detail -----------------------------------------------
struct SavedState;
bool Begin(ID3D11DeviceContext* ctx, SavedState* saved);
void End(ID3D11DeviceContext* ctx, SavedState* saved);

struct SavedState {
  ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11ClassInstance* ps_instances[D3D11_SHADER_MAX_INTERFACES] = {};
  UINT ps_instance_count = 0;
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11BlendState* blend = nullptr;
  FLOAT blend_factor[4] = {};
  UINT sample_mask = 0;
  D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
  UINT viewport_count = 0;
  D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
  UINT scissor_count = 0;
};

template <typename Draw>
void Redraw(ID3D11DeviceContext* ctx, Draw draw) {
  SavedState saved;
  if (!Begin(ctx, &saved)) return;
  draw();
  End(ctx, &saved);
}

}  // namespace ds3::object_motion
