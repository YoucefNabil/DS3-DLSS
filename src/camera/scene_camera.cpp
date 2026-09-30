#include "camera/scene_camera.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "common/config.h"
#include "common/log.h"

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

std::atomic<uint32_t> g_frame{0};
std::atomic<bool> g_dlss_jitter{false};

std::mutex g_snapshot_mutex;
Snapshot g_snapshot;
bool g_have_snapshot = false;

float Halton(uint32_t index, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  for (; index; index /= base) {
    f /= static_cast<float>(base);
    r += f * static_cast<float>(index % base);
  }
  return r;
}

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

bool IsSceneCb(ID3D11Resource* r) {
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  r->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
  D3D11_BUFFER_DESC d;
  static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
  return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && d.ByteWidth == kSceneCbBytes;
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
  g_have_snapshot = true;
}

}  // namespace

const void* OnConstantUpload(ID3D11Resource* dst, const D3D11_BOX* box, const void* data,
                             void* scratch) {
  const Config& cfg = GetConfig();
  const bool dlss = g_dlss_jitter.load(std::memory_order_relaxed);
  if (!(cfg.jitter || cfg.dlss) || !dst || !data) return data;
  if (box && (box->left != 0 || box->right < kSceneCbBytes)) return data;
  if (!IsSceneCb(dst) || !IsMainCamera(static_cast<const float*>(data))) return data;

  const uint32_t frame = g_frame.load(std::memory_order_relaxed);
  const float* in = static_cast<const float*>(data);
  if (!(cfg.jitter || dlss)) {
    Remember(in, frame, 0.0f, 0.0f);
    return data;
  }

  const uint32_t n = frame % 32 + 1;
  // Pixel offsets in [-0.5, 0.5); NDC y points up, pixel y down.
  const float scale = dlss ? 1.0f : cfg.jitter_debug_scale;
  const float ox = (Halton(n, 2) - 0.5f) * scale;
  const float oy = (Halton(n, 3) - 0.5f) * scale;
  Remember(in, frame, ox, oy);

  std::memcpy(scratch, data, kSceneCbBytes);
  float* cb = static_cast<float*>(scratch);
  const float jx = 2.0f * ox / cb[kScreenSize];
  const float jy = -2.0f * oy / cb[kScreenSize + 1];
  JitterMatrix(cb + kProj, jx, jy);
  JitterMatrix(cb + kViewProj, jx, jy);
  JitterInverse(cb + kInvProj, jx, jy);
  JitterInverse(cb + kInvViewProj, jx, jy);
  return scratch;
}

bool LatestMainCamera(Snapshot* out) {
  std::lock_guard lock(g_snapshot_mutex);
  if (!g_have_snapshot) return false;
  *out = g_snapshot;
  return true;
}

uint32_t Frame() { return g_frame.load(std::memory_order_relaxed); }

void SetDlssJitter(bool on) { g_dlss_jitter.store(on, std::memory_order_relaxed); }

void OnPresent() { g_frame.fetch_add(1, std::memory_order_relaxed); }

}  // namespace ds3::camera
