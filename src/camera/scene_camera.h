#pragma once
#include <d3d11.h>

#include <cstdint>

// Sub-pixel camera jitter applied through DS3's own constant-buffer upload:
// every view's cbSceneParam (960 bytes, register b8) is written with
// UpdateSubresource, so the main camera's matrices can be offset in the data
// passed to D3D11. No game code or memory is modified.
namespace ds3::camera {

constexpr UINT kSceneCbBytes = 960;
// cbMatrixWVP (b0), the forward shaders' copy of the view-projection. The
// depth pre-pass of fading objects uses it; it gets the same jitter.
constexpr UINT kViewCbBytes = 192;

// The main camera as the game uploaded it (before our jitter), plus the
// jitter we applied on top. Matrices are rows of M for clip = M * v.
struct Snapshot {
  uint32_t frame = 0;          // Present count when it was uploaded
  float width = 0, height = 0;
  float jitter_x = 0, jitter_y = 0;  // content shift in pixels (x right, y down)
  float inv_view_proj[16] = {};      // NDC -> absolute world
  float view_proj[16] = {};          // camera-relative world -> clip
  float camera_pos[3] = {};
  float proj_z = 0;  // FC_MatrixProj[10]: depth = proj_z - near / view distance
  float near_plane = 0;  // view distance of depth 0
  float fov_y = 0;       // vertical field of view, radians
};

// Returns the data to upload: `data` unchanged, or `scratch` (kSceneCbBytes)
// holding a jittered copy when dst is the main camera's cbSceneParam or
// cbMatrixWVP.
const void* OnConstantUpload(ID3D11Resource* dst, const D3D11_BOX* box, const void* data,
                             void* scratch);

// Map/Unmap path of cbMatrixWVP: jittered in the mapped memory at Unmap.
void OnMap(ID3D11DeviceContext* ctx, ID3D11Resource* r, D3D11_MAP type, void* data);
void OnUnmap(ID3D11DeviceContext* ctx, ID3D11Resource* r);
uint32_t TakeViewUploadCount();  // cbMatrixWVP uploads jittered since the last call

// The jitter the main camera gets this frame, in pixels (x right, y down);
// false when nothing is jittered.
bool CurrentJitter(float* ox, float* oy);

bool LatestMainCamera(Snapshot* out);  // false until the main camera was seen
uint32_t Frame();
void SetDlssJitter(bool on);  // jitter for DLSS, independent of [Jitter] Enabled
void OnPresent();             // advances the jitter sequence
// The swap chain's size. Only a cbSceneParam of this size is the main camera;
// smaller copies (a half-resolution pass) are jittered to match but never
// become the camera DLAA uses.
void SetOutputSize(UINT width, UINT height);

}  // namespace ds3::camera
