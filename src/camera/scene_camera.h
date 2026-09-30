#pragma once
#include <d3d11.h>

#include <cstdint>

// Sub-pixel camera jitter applied through DS3's own constant-buffer upload:
// every view's cbSceneParam (960 bytes, register b8) is written with
// UpdateSubresource, so the main camera's matrices can be offset in the data
// passed to D3D11. No game code or memory is modified.
namespace ds3::camera {

constexpr UINT kSceneCbBytes = 960;

// The main camera as the game uploaded it (before our jitter), plus the
// jitter we applied on top. Matrices are rows of M for clip = M * v.
struct Snapshot {
  uint32_t frame = 0;          // Present count when it was uploaded
  float width = 0, height = 0;
  float jitter_x = 0, jitter_y = 0;  // content shift in pixels (x right, y down)
  float inv_view_proj[16] = {};      // NDC -> absolute world
  float view_proj[16] = {};          // camera-relative world -> clip
  float camera_pos[3] = {};
};

// Returns the data to upload: `data` unchanged, or `scratch` (kSceneCbBytes)
// holding a jittered copy when dst is the main camera's cbSceneParam.
const void* OnConstantUpload(ID3D11Resource* dst, const D3D11_BOX* box, const void* data,
                             void* scratch);

bool LatestMainCamera(Snapshot* out);  // false until the main camera was seen
uint32_t Frame();
void SetDlssJitter(bool on);  // jitter for DLSS, independent of [Jitter] Enabled
void OnPresent();             // advances the jitter sequence

}  // namespace ds3::camera
