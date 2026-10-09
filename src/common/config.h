#pragma once

namespace ds3 {

struct Config {
  bool enabled = true;         // master switch: 0 = pure pass-through proxy, no hooks
  // [Debug] Enabled: 0 = every [Debug] setting below is off (hotkeys, dumps,
  // captures, DebugView), whatever the ini says.
  bool debug = false;
  int dump_frame_key = 0x79;   // VK_F10: write the next frame's pass list to a file
  int dump_frame_at = 0;       // >0: also dump automatically after this many Presents
  bool dump_shaders = false;   // write each shader's bytecode into the DS3_DLSS_shaders folder
  int capture_key = 0x78;      // VK_F9: save the next CaptureFrames frames of AA inputs
  int capture_frames = 48;     // frames per capture
  int capture_at = 0;          // >0: also capture automatically from this frame on
  bool jitter = false;               // [Jitter] Enabled: sub-pixel offset without DLSS (test)
  float jitter_debug_scale = 1.0f;   // [Jitter] DebugScale: >1 exaggerates it for testing
  bool dlss = true;                  // [DLSS] Enabled: DLAA on the HDR scene (implies jitter)
  int dlss_preset = 0;               // [DLSS] Preset: 0 = NGX default, else preset letter (J = 10)
  float dlss_jitter_sign = 1.0f;     // [DLSS] JitterSign: +1 or -1, how the offset is reported
  int dlss_toggle_key = 0x24;        // [DLSS] ToggleKey: VK_HOME switches DLAA + jitter on/off
  bool dlss_object_motion = true;  // per-object motion vectors from the velocity pass
  float dlss_reactive = 1.0f;  // reactive mask strength for transparent effects (0 = off)
  float dlss_mip_bias = -0.5f;  // [DLSS] MipBias: texture LOD bias while DLAA runs (0 = off)
  int dlss_mip_bias_key = 0x23;  // [Debug] MipBiasKey: VK_END switches the mip bias on/off
  int dlss_debug_view = 0;       // [Debug] DebugView: 1 = show motion vectors instead
  bool widescreen = false;           // [Widescreen] Enabled: offer any resolution in game
  int widescreen_width = 0;          // [Widescreen] Width/Height: 0 = display resolution
  int widescreen_height = 0;
  bool widescreen_follow_display = true;  // [Widescreen] FollowDisplay: start at that size
  bool frame_rate_unlock = false;    // [FrameRate] Unlock: replace the 60 fps cap
  float frame_rate_fps = 120.0f;     // [FrameRate] FPS: the new cap
  bool window_borderless = false;    // [Window] Borderless: windowed mode without a frame
};

const Config& GetConfig();
void LoadConfig();  // reads DS3_DLSS.ini next to the DLL

}  // namespace ds3
