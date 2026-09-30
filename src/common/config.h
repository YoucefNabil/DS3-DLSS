#pragma once

namespace ds3 {

struct Config {
  bool enabled = true;         // master switch: 0 = pure pass-through proxy, no hooks
  int dump_frame_key = 0x79;   // VK_F10: write the next frame's pass list to a file
  int dump_frame_at = 0;       // >0: also dump automatically after this many Presents
  bool dump_shaders = false;   // write each shader's bytecode into the DS3_DLSS_shaders folder
  bool jitter = false;               // [Jitter] Enabled: sub-pixel offset without DLSS (test)
  float jitter_debug_scale = 1.0f;   // [Jitter] DebugScale: >1 exaggerates it for testing
  bool dlss = true;                  // [DLSS] Enabled: DLAA on the HDR scene (implies jitter)
  int dlss_preset = 0;               // [DLSS] Preset: 0 = NGX default, else preset letter (J = 10)
  float dlss_jitter_sign = 1.0f;     // [DLSS] JitterSign: +1 or -1, how the offset is reported
  int dlss_toggle_key = 0x24;        // [DLSS] ToggleKey: VK_HOME switches DLAA + jitter on/off
  int dlss_debug_view = 0;           // [DLSS] DebugView: 1 = show motion vectors instead
};

const Config& GetConfig();
void LoadConfig();  // reads DS3_DLSS.ini next to the DLL

}  // namespace ds3
