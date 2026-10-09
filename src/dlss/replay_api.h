#pragma once
// DS3_DLSS_ReplayFrame, exported by the proxy for tools/capture_replay: runs
// one captured frame through exactly the code the game frame went through
// (motion vectors, reactive mask, then DLSS with the settings of the DS3_DLSS.ini
// next to the DLL), without the game. Frames must come in order; the first
// one, and any after a gap, reset the history as in game.

#include <d3d11.h>

#include <cstdint>

struct DS3ReplayFrame {
  uint32_t size;  // sizeof(DS3ReplayFrame)
  // Inputs. color is RGBA16F with SRV binding: in, the scene; out, the result
  // as the game would have shown it.
  ID3D11Texture2D* color;
  ID3D11Texture2D* depth;                   // R32G8X24_TYPELESS with SRV binding
  ID3D11ShaderResourceView* object_motion;  // null when the capture has none
  ID3D11ShaderResourceView* effects;        // null when the capture has none
  // The frame's camera (see camera::Snapshot) and frame time.
  uint32_t frame;
  float width, height;
  float jitter_x, jitter_y;
  float inv_view_proj[16];
  float view_proj[16];
  float camera_pos[3];
  float proj_z, near_plane, fov_y;
  float frame_ms;
  // Optional: receive what the backend was given (same formats as g_t).
  ID3D11Texture2D* motion_out;  // R16G16F
  ID3D11Texture2D* bias_out;    // R8
  ID3D11Texture2D* object_out;  // R8
  // Out.
  int32_t reset;  // the history was reset this frame
};

// 1 = the backend ran, 0 = it did not (see DS3_DLSS.log next to the DLL).
using DS3ReplayFrameFn = int(__cdecl*)(ID3D11DeviceContext* ctx, DS3ReplayFrame* frame);
