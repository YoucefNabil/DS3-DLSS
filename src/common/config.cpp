#include "common/config.h"

#include <windows.h>
#include <cmath>

#include "core.h"

namespace ds3 {
namespace {
Config g_config;
}

const Config& GetConfig() { return g_config; }

void LoadConfig() {
  const std::wstring ini = ModuleDir() + L"DS3_DLSS.ini";
  g_config.enabled = GetPrivateProfileIntW(L"General", L"Enabled", 1, ini.c_str()) != 0;
  g_config.debug = GetPrivateProfileIntW(L"Debug", L"Enabled", 0, ini.c_str()) != 0;
  wchar_t key[32] = {};
  GetPrivateProfileStringW(L"Debug", L"DumpFrameKey", L"0x79", key, 32, ini.c_str());
  g_config.dump_frame_key = static_cast<int>(wcstol(key, nullptr, 0));  // accepts hex or decimal
  g_config.dump_frame_at = GetPrivateProfileIntW(L"Debug", L"DumpFrameAt", 0, ini.c_str());
  g_config.dump_shaders = GetPrivateProfileIntW(L"Debug", L"DumpShaders", 0, ini.c_str()) != 0;
  GetPrivateProfileStringW(L"Debug", L"CaptureKey", L"0x78", key, 32, ini.c_str());
  g_config.capture_key = static_cast<int>(wcstol(key, nullptr, 0));
  g_config.capture_frames = GetPrivateProfileIntW(L"Debug", L"CaptureFrames", 48, ini.c_str());
  g_config.capture_at = GetPrivateProfileIntW(L"Debug", L"CaptureAt", 0, ini.c_str());
  g_config.jitter = GetPrivateProfileIntW(L"Jitter", L"Enabled", 0, ini.c_str()) != 0;
  wchar_t scale[32] = {};
  GetPrivateProfileStringW(L"Jitter", L"DebugScale", L"1.0", scale, 32, ini.c_str());
  g_config.jitter_debug_scale = static_cast<float>(wcstod(scale, nullptr));
  g_config.dlss = GetPrivateProfileIntW(L"DLSS", L"Enabled", 1, ini.c_str()) != 0;
  wchar_t preset[32] = {};
  GetPrivateProfileStringW(L"DLSS", L"Preset", L"Default", preset, 32, ini.c_str());
  // A single letter names the preset (A = 1 ... K = 11), anything else is NGX's default.
  if (preset[0] && !preset[1] && ((preset[0] | 0x20) >= L'a' && (preset[0] | 0x20) <= L'z'))
    g_config.dlss_preset = (preset[0] | 0x20) - L'a' + 1;
  // Read as text: GetPrivateProfileInt turns negative numbers into 0.
  wchar_t sign[32] = {};
  GetPrivateProfileStringW(L"DLSS", L"JitterSign", L"1", sign, 32, ini.c_str());
  g_config.dlss_jitter_sign = wcstol(sign, nullptr, 10) < 0 ? -1.0f : 1.0f;
  GetPrivateProfileStringW(L"DLSS", L"ToggleKey", L"0x24", key, 32, ini.c_str());
  g_config.dlss_toggle_key = static_cast<int>(wcstol(key, nullptr, 0));
  g_config.dlss_debug_view = GetPrivateProfileIntW(L"Debug", L"DebugView", 0, ini.c_str());
  g_config.dlss_object_motion =
      GetPrivateProfileIntW(L"DLSS", L"ObjectMotion", 1, ini.c_str()) != 0;
  wchar_t reactive[32] = {};
  GetPrivateProfileStringW(L"DLSS", L"Reactive", L"1.0", reactive, 32, ini.c_str());
  g_config.dlss_reactive = std::fmax(0.0f, static_cast<float>(_wtof(reactive)));
  wchar_t mip_bias[32] = {};
  GetPrivateProfileStringW(L"DLSS", L"MipBias", L"-0.5", mip_bias, 32, ini.c_str());
  g_config.dlss_mip_bias =
      std::fmin(0.0f, std::fmax(-3.0f, static_cast<float>(_wtof(mip_bias))));
  GetPrivateProfileStringW(L"Debug", L"MipBiasKey", L"0x23", key, 32, ini.c_str());
  g_config.dlss_mip_bias_key = static_cast<int>(wcstol(key, nullptr, 0));
  if (!g_config.debug) {
    g_config.dump_frame_key = 0;
    g_config.dump_frame_at = 0;
    g_config.dump_shaders = false;
    g_config.capture_key = 0;
    g_config.capture_at = 0;
    g_config.dlss_mip_bias_key = 0;
    g_config.dlss_debug_view = 0;
  }
  g_config.widescreen = GetPrivateProfileIntW(L"Widescreen", L"Enabled", 0, ini.c_str()) != 0;
  g_config.widescreen_width = GetPrivateProfileIntW(L"Widescreen", L"Width", 0, ini.c_str());
  g_config.widescreen_height = GetPrivateProfileIntW(L"Widescreen", L"Height", 0, ini.c_str());
  g_config.widescreen_follow_display =
      GetPrivateProfileIntW(L"Widescreen", L"FollowDisplay", 1, ini.c_str()) != 0;
  g_config.frame_rate_unlock = GetPrivateProfileIntW(L"FrameRate", L"Unlock", 0, ini.c_str()) != 0;
  wchar_t fps[32] = {};
  GetPrivateProfileStringW(L"FrameRate", L"FPS", L"120", fps, 32, ini.c_str());
  g_config.frame_rate_fps = std::fmin(1000.0f, std::fmax(20.0f, static_cast<float>(_wtof(fps))));
  g_config.window_borderless = GetPrivateProfileIntW(L"Window", L"Borderless", 0, ini.c_str()) != 0;
  Log("config: Enabled=%d Debug=%d DumpFrameKey=0x%02X DumpFrameAt=%d DumpShaders=%d "
      "CaptureKey=0x%02X CaptureFrames=%d CaptureAt=%d MipBiasKey=0x%02X DebugView=%d Jitter=%d "
      "JitterDebugScale=%.2f",
      g_config.enabled, g_config.debug, g_config.dump_frame_key, g_config.dump_frame_at,
      g_config.dump_shaders, g_config.capture_key, g_config.capture_frames, g_config.capture_at,
      g_config.dlss_mip_bias_key, g_config.dlss_debug_view, g_config.jitter,
      g_config.jitter_debug_scale);
  Log("config: DLSS=%d Preset=%d JitterSign=%+.0f ToggleKey=0x%02X ObjectMotion=%d"
      " Reactive=%.2f MipBias=%.2f",
      g_config.dlss, g_config.dlss_preset, g_config.dlss_jitter_sign, g_config.dlss_toggle_key,
      g_config.dlss_object_motion, g_config.dlss_reactive,
      g_config.dlss_mip_bias);
  Log("config: Widescreen=%d Width=%d Height=%d FollowDisplay=%d", g_config.widescreen,
      g_config.widescreen_width, g_config.widescreen_height, g_config.widescreen_follow_display);
  Log("config: FrameRate Unlock=%d FPS=%.1f, Window Borderless=%d", g_config.frame_rate_unlock,
      g_config.frame_rate_fps, g_config.window_borderless);

  dlaa_core::CoreConfig& core = dlaa_core::MutableCoreConfig();
  core.enabled = g_config.enabled;
  core.debug_tools = g_config.debug;
  core.dump_frame_key = g_config.dump_frame_key;
  core.dump_frame_at = g_config.dump_frame_at;
  core.dump_shaders = g_config.dump_shaders;
  core.capture_frames = g_config.capture_frames;
  core.mip_bias = g_config.dlss ? g_config.dlss_mip_bias : 0.0f;
}

}  // namespace ds3
