// Dark Souls III on dlaa-core: config, the opt-in exe tweaks, hotkeys, and the
// core hook callbacks routed to the camera, DLAA, object-motion and reactive
// code.
#include "dlaa_core/game.h"

#include "camera/scene_camera.h"
#include "common/config.h"
#include "core.h"
#include "dlaa_core/hooks.h"
#include "dlss/capture.h"
#include "dlss/dlaa.h"
#include "dlss/object_motion.h"
#include "dlss/reactive.h"
#include "tweaks/frame_rate.h"
#include "tweaks/window.h"
#include "widescreen/widescreen.h"

namespace ds3 {
namespace {

namespace hooks = dlaa_core::hooks;

void OnCreateShader(ID3D11Device*, hooks::Stage stage, ID3D11DeviceChild* shader, uint64_t hash,
                    const void* code, size_t length) {
  if (stage == hooks::Stage::kVertex)
    object_motion::OnVertexShaderCreated(static_cast<ID3D11VertexShader*>(shader), code, length,
                                         hash);
}

void OnVsSetShader(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs) {
  object_motion::OnSetVertexShader(ctx, vs);
}

// Output-merger state, for the reactive mask (see dlss/reactive.h).
void OnSetRenderTargets(ID3D11DeviceContext* ctx, UINT count, ID3D11RenderTargetView* const* rtvs,
                        ID3D11DepthStencilView*) {
  reactive::OnSetRenderTargets(ctx, count, rtvs);
}

void OnSetBlendState(ID3D11DeviceContext* ctx, ID3D11BlendState* blend, const FLOAT*, UINT) {
  reactive::OnSetBlendState(ctx, blend);
}

void OnSetDepthStencilState(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* state, UINT) {
  reactive::OnSetDepthStencilState(ctx, state);
}

// ClearState, or a command list finished or executed without restoring state:
// the context is back to defaults, with no shaders bound.
void OnResetState(ID3D11DeviceContext* ctx) {
  reactive::OnResetState(ctx);
  object_motion::OnSetVertexShader(ctx, nullptr);
}

uint32_t BeforeDraw(ID3D11DeviceContext* ctx, const hooks::DrawArgs& args) {
  if (args.kind == hooks::DrawArgs::kDraw) dlaa::BeforeDraw(ctx);
  return object_motion::WantsRedraw(ctx) || reactive::WantsRedraw(ctx) ? hooks::kAfter
                                                                      : hooks::kProceed;
}

// Draws after the game's own: motion-blur velocity draws are repeated into the
// full-resolution object-motion target (dlss/object_motion.h), effect draws
// into the reactive mask (dlss/reactive.h).
void AfterDraw(ID3D11DeviceContext* ctx, const hooks::DrawArgs& args) {
  if (object_motion::WantsRedraw(ctx))
    object_motion::Redraw(ctx, [&] { hooks::Reissue(ctx, args); });
  if (reactive::WantsRedraw(ctx)) reactive::Redraw(ctx, [&] { hooks::Reissue(ctx, args); });
}

void OnClearDsv(ID3D11DeviceContext*, ID3D11DepthStencilView* dsv, UINT, FLOAT, UINT8) {
  dlaa::OnClearDepth(dsv);
}

void OnMap(ID3D11DeviceContext* ctx, ID3D11Resource* r, D3D11_MAP type, void* data) {
  camera::OnMap(ctx, r, type, data);
}

void OnUnmap(ID3D11DeviceContext* ctx, ID3D11Resource* r) { camera::OnUnmap(ctx, r); }

// Jitters the camera constant buffers on their way to the GPU.
const void* OnUpdateSubresource(ID3D11DeviceContext*, ID3D11Resource* r, UINT,
                                const D3D11_BOX* box, const void* data) {
  alignas(16) thread_local uint8_t scratch[camera::kSceneCbBytes];
  return camera::OnConstantUpload(r, box, data, scratch);
}

void OnPresent(IDXGISwapChain* swap_chain, const DXGI_SWAP_CHAIN_DESC& desc) {
  camera::SetOutputSize(desc.BufferDesc.Width, desc.BufferDesc.Height);
  camera::OnPresent();
  dlaa::OnPresent(swap_chain);
}

void OnFullscreenState(IDXGISwapChain*, BOOL fullscreen) { window::OnFullscreenState(fullscreen); }

}  // namespace
}  // namespace ds3

namespace dlaa_core::game {

const wchar_t* Prefix() { return L"DS3_DLSS"; }

bool OnAttach() {
  using namespace ds3;
  LoadConfig();
  // Opt-in exe tweaks, applied even with [General] Enabled=0.
  widescreen::Apply();  // before the game's own code reads its resolution list
  window::Apply();      // before the game creates its window
  frame_rate::Apply();  // waits on its own thread for the game to start up

  hooks::Callbacks cb;
  cb.on_create_shader = &OnCreateShader;
  cb.on_vs_set_shader = &OnVsSetShader;
  cb.on_set_render_targets = &OnSetRenderTargets;
  cb.on_set_blend_state = &OnSetBlendState;
  cb.on_set_depth_stencil_state = &OnSetDepthStencilState;
  cb.on_clear_state = &OnResetState;
  cb.on_command_list_reset = &OnResetState;
  cb.before_draw = &BeforeDraw;
  cb.after_draw = &AfterDraw;
  cb.on_clear_dsv = &OnClearDsv;
  cb.on_map = &OnMap;
  cb.on_unmap = &OnUnmap;
  cb.on_update_subresource = &OnUpdateSubresource;
  cb.on_present = &OnPresent;
  cb.on_fullscreen_state = &OnFullscreenState;
  hooks::SetCallbacks(cb);
  // Home always works; the other keys are 0 unless [Debug] Enabled=1.
  const Config& c = GetConfig();
  hooks::AddHotkey(c.dlss_toggle_key, false, &dlaa::Toggle);
  hooks::AddHotkey(c.dlss_mip_bias_key, true, &mip_bias::Toggle);
  hooks::AddHotkey(c.capture_key, true, &capture::Request);
  return true;
}

}  // namespace dlaa_core::game
