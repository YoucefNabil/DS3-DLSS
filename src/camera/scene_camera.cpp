#include "camera/scene_camera.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>

#include "common/config.h"
#include "core.h"

namespace ds3::camera {
namespace {

// cbSceneParam layout (float offsets), from the shaders' reflection data.
// Matrices are stored as rows of M for clip = M * v.
constexpr int kScreenSize = 16 / 4;   // float4 (w, h, 1/w, 1/h)
constexpr int kCameraPos = 48 / 4;
constexpr int kInvViewProj = 128 / 4;
constexpr int kInvProj = 192 / 4;
constexpr int kProj = 256 / 4;
constexpr int kViewProj = 384 / 4;  // camera-relative (no translation)

// cbMatrixWVP (192 bytes, register b0) in its per-view layout: the depth
// pre-pass and other forward shaders transform with this copy of the camera.
constexpr int kViewCamera = 0;         // row_major camera-to-world, row 3 = position
constexpr int kViewViewProj = 64 / 4;  // same matrix as cbSceneParam's ViewProj

std::atomic<uint32_t> g_frame{0};
std::atomic<bool> g_dlss_jitter{false};

std::atomic<uint32_t> g_view_uploads{0};
std::atomic<float> g_screen_w{0.0f}, g_screen_h{0.0f};
std::atomic<float> g_output_w{0.0f}, g_output_h{0.0f};  // swap chain
std::atomic<bool> g_logged_small{false};
std::atomic<bool> g_logged_map_full{false};

// Map/Unmap of cbMatrixWVP buffers, finished at Unmap.
std::mutex g_map_mutex;
struct PendingMap {
  ID3D11DeviceContext* ctx;
  ID3D11Resource* r;
  float* data;
};
PendingMap g_maps[8];

std::mutex g_snapshot_mutex;
Snapshot g_snapshot;
bool g_have_snapshot = false;

// Perspective view whose aspect matches the screen. Shadow cascades are
// orthographic, and the other perspective views (e.g. 512x512 spot/env maps)
// are square.
bool IsMainCamera(const float* cb) {
  const float w = cb[kScreenSize], h = cb[kScreenSize + 1];
  const float* proj = cb + kProj;
  if (!(w > 0.0f && h > 0.0f) || proj[0] == 0.0f) return false;
  if (proj[12] != 0.0f || proj[13] != 0.0f || proj[14] != 1.0f || proj[15] != 0.0f) return false;
  const float aspect = w / h;
  return std::fabs(proj[5] / proj[0] - aspect) < 0.01f * aspect;
}

// clip' = J * clip with J = translate(jx, jy) scaled by w.
void JitterMatrix(float* m, float jx, float jy) {
  for (int c = 0; c < 4; ++c) {
    m[c] += jx * m[12 + c];
    m[4 + c] += jy * m[12 + c];
  }
}

// inv' = inv * J^-1
void JitterInverse(float* m, float jx, float jy) {
  for (int r = 0; r < 4; ++r) m[r * 4 + 3] -= jx * m[r * 4] + jy * m[r * 4 + 1];
}

bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

// The per-view cbMatrixWVP of the main camera: a rigid camera matrix, and a
// camera-relative perspective view-projection with the screen's aspect. The
// velocity pass binds a buffer of the same size with object matrices
// (WVP, previous WVP), which fails both tests and is never touched.
bool IsMainViewCb(const float* cb, float w, float h) {
  const float* cam = cb + kViewCamera;
  if (cam[3] != 0.0f || cam[7] != 0.0f || cam[11] != 0.0f || cam[15] != 1.0f) return false;
  const float* vp = cb + kViewViewProj;
  const float fwd = std::sqrt(vp[12] * vp[12] + vp[13] * vp[13] + vp[14] * vp[14]);
  if (!Near(fwd, 1.0f, 1e-3f) || !Near(vp[15], 0.0f, 1e-4f)) return false;
  const float sx = std::sqrt(vp[0] * vp[0] + vp[1] * vp[1] + vp[2] * vp[2]);
  const float sy = std::sqrt(vp[4] * vp[4] + vp[5] * vp[5] + vp[6] * vp[6]);
  if (!(sx > 0.0f && w > 0.0f && h > 0.0f)) return false;
  const float aspect = w / h;
  return std::fabs(sy / sx - aspect) < 0.01f * aspect;
}

// This frame's jitter in pixels, or false when there is none.
bool FrameJitter(float* ox, float* oy) {
  const Config& cfg = GetConfig();
  const bool dlss = g_dlss_jitter.load(std::memory_order_relaxed);
  if (!(cfg.jitter || dlss)) return false;
  const uint32_t n = g_frame.load(std::memory_order_relaxed) % 32 + 1;
  // Pixel offsets in [-0.5, 0.5); NDC y points up, pixel y down.
  const float scale = dlss ? 1.0f : cfg.jitter_debug_scale;
  *ox = (Halton(n, 2) - 0.5f) * scale;
  *oy = (Halton(n, 3) - 0.5f) * scale;
  return true;
}

// A copy of a cbSceneParam in `scratch` with the NDC jitter in all four of its
// camera matrices.
const void* JitteredSceneCb(const void* data, void* scratch, float jx, float jy) {
  std::memcpy(scratch, data, kSceneCbBytes);
  float* cb = static_cast<float*>(scratch);
  JitterMatrix(cb + kProj, jx, jy);
  JitterMatrix(cb + kViewProj, jx, jy);
  JitterInverse(cb + kInvProj, jx, jy);
  JitterInverse(cb + kInvViewProj, jx, jy);
  return scratch;
}

// Jitters a main-camera cbMatrixWVP in place, like cbSceneParam's ViewProj, so
// the depth pre-pass and the shading pass agree on every pixel's depth.
bool JitterViewCb(float* cb) {
  const float w = g_screen_w.load(std::memory_order_relaxed);
  const float h = g_screen_h.load(std::memory_order_relaxed);
  float ox, oy;
  if (!IsMainViewCb(cb, w, h) || !FrameJitter(&ox, &oy)) return false;
  JitterMatrix(cb + kViewViewProj, 2.0f * ox / w, -2.0f * oy / h);
  g_view_uploads.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// All main-camera uploads of one frame carry the same matrices; the first one
// of each frame is kept.
void Remember(const float* cb, uint32_t frame, float ox, float oy) {
  std::lock_guard lock(g_snapshot_mutex);
  if (g_have_snapshot && g_snapshot.frame == frame) return;
  g_snapshot.frame = frame;
  g_snapshot.width = cb[kScreenSize];
  g_snapshot.height = cb[kScreenSize + 1];
  g_snapshot.jitter_x = ox;
  g_snapshot.jitter_y = oy;
  std::memcpy(g_snapshot.inv_view_proj, cb + kInvViewProj, sizeof(g_snapshot.inv_view_proj));
  std::memcpy(g_snapshot.view_proj, cb + kViewProj, sizeof(g_snapshot.view_proj));
  std::memcpy(g_snapshot.camera_pos, cb + kCameraPos, sizeof(g_snapshot.camera_pos));
  const float* proj = cb + kProj;
  g_snapshot.proj_z = proj[10];
  g_snapshot.near_plane = proj[10] != 0.0f ? -proj[11] / proj[10] : 0.0f;
  g_snapshot.fov_y = proj[5] > 0.0f ? 2.0f * std::atan(1.0f / proj[5]) : 0.0f;
  g_have_snapshot = true;
}

}  // namespace

const void* OnConstantUpload(ID3D11Resource* dst, const D3D11_BOX* box, const void* data,
                             void* scratch) {
  const Config& cfg = GetConfig();
  if (!(cfg.jitter || cfg.dlss) || !dst || !data) return data;
  if (box && box->left != 0) return data;
  const UINT bytes = ConstantBufferBytes(dst);
  if (bytes == kViewCbBytes) {
    if (box && box->right < kViewCbBytes) return data;
    std::memcpy(scratch, data, kViewCbBytes);
    return JitterViewCb(static_cast<float*>(scratch)) ? scratch : data;
  }
  if (bytes != kSceneCbBytes || (box && box->right < kSceneCbBytes)) return data;
  if (!IsMainCamera(static_cast<const float*>(data))) return data;

  const uint32_t frame = g_frame.load(std::memory_order_relaxed);
  const float* in = static_cast<const float*>(data);
  const float out_w = g_output_w.load(std::memory_order_relaxed);
  const float out_h = g_output_h.load(std::memory_order_relaxed);
  if (out_w > 0.0f && (in[kScreenSize] != out_w || in[kScreenSize + 1] != out_h)) {
    // The same camera for a smaller target (some areas draw a 960x540 pass).
    // It gets the full-resolution jitter in NDC so it lines up with the scene,
    // but it must not replace the main camera: DLAA would then see a size
    // mismatch and skip, leaving the jitter visible.
    if (!g_logged_small.exchange(true))
      Log("jitter: %.0fx%.0f copy of the main camera (screen %.0fx%.0f): jittered to match, "
          "not used as the main camera", in[kScreenSize], in[kScreenSize + 1], out_w, out_h);
    float ox, oy;
    const float w = g_screen_w.load(std::memory_order_relaxed);
    const float h = g_screen_h.load(std::memory_order_relaxed);
    if (!(w > 0.0f && h > 0.0f) || !FrameJitter(&ox, &oy)) return data;
    return JitteredSceneCb(data, scratch, 2.0f * ox / w, -2.0f * oy / h);
  }
  g_screen_w.store(in[kScreenSize], std::memory_order_relaxed);
  g_screen_h.store(in[kScreenSize + 1], std::memory_order_relaxed);
  float ox, oy;
  if (!FrameJitter(&ox, &oy)) {
    Remember(in, frame, 0.0f, 0.0f);
    return data;
  }
  Remember(in, frame, ox, oy);
  return JitteredSceneCb(data, scratch, 2.0f * ox / in[kScreenSize],
                         -2.0f * oy / in[kScreenSize + 1]);
}

void OnMap(ID3D11DeviceContext* ctx, ID3D11Resource* r, D3D11_MAP type, void* data) {
  const Config& cfg = GetConfig();
  if (!(cfg.jitter || cfg.dlss) || !data || type != D3D11_MAP_WRITE_DISCARD) return;
  if (ConstantBufferBytes(r) != kViewCbBytes) return;
  {
    std::lock_guard lock(g_map_mutex);
    for (PendingMap& m : g_maps) {
      if (m.data) continue;
      m = {ctx, r, static_cast<float*>(data)};
      return;
    }
  }
  // This buffer goes to the GPU unjittered: whatever draws with it flickers.
  if (!g_logged_map_full.exchange(true))
    Log("jitter: more than %zu cbMatrixWVP buffers mapped at once, the extra ones are not "
        "jittered (logged once)", std::size(g_maps));
}

void OnUnmap(ID3D11DeviceContext* ctx, ID3D11Resource* r) {
  float* data = nullptr;
  {
    std::lock_guard lock(g_map_mutex);
    for (PendingMap& m : g_maps) {
      if (!m.data || m.ctx != ctx || m.r != r) continue;
      data = m.data;
      m = {};
      break;
    }
  }
  // The game has written the whole buffer; this is still the driver's memory.
  if (data) JitterViewCb(data);
}

bool CurrentJitter(float* ox, float* oy) { return FrameJitter(ox, oy); }

uint32_t TakeViewUploadCount() { return g_view_uploads.exchange(0, std::memory_order_relaxed); }

bool LatestMainCamera(Snapshot* out) {
  std::lock_guard lock(g_snapshot_mutex);
  if (!g_have_snapshot) return false;
  *out = g_snapshot;
  return true;
}

uint32_t Frame() { return g_frame.load(std::memory_order_relaxed); }

void SetDlssJitter(bool on) { g_dlss_jitter.store(on, std::memory_order_relaxed); }

void OnPresent() { g_frame.fetch_add(1, std::memory_order_relaxed); }

void SetOutputSize(UINT width, UINT height) {
  g_output_w.store(static_cast<float>(width), std::memory_order_relaxed);
  g_output_h.store(static_cast<float>(height), std::memory_order_relaxed);
}

}  // namespace ds3::camera
