#include "common/config.h"

#include <windows.h>

#include "common/log.h"

namespace ds3 {
namespace {
Config g_config;
}

const Config& GetConfig() { return g_config; }

void LoadConfig() {
  const std::wstring ini = ModuleDir() + L"DS3_DLSS.ini";
  g_config.enabled = GetPrivateProfileIntW(L"General", L"Enabled", 1, ini.c_str()) != 0;
  wchar_t key[32] = {};
  GetPrivateProfileStringW(L"Debug", L"DumpFrameKey", L"0x79", key, 32, ini.c_str());
  g_config.dump_frame_key = static_cast<int>(wcstol(key, nullptr, 0));  // accepts hex or decimal
  g_config.dump_frame_at = GetPrivateProfileIntW(L"Debug", L"DumpFrameAt", 0, ini.c_str());
  g_config.dump_shaders = GetPrivateProfileIntW(L"Debug", L"DumpShaders", 0, ini.c_str()) != 0;
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
  g_config.dlss_debug_view = GetPrivateProfileIntW(L"DLSS", L"DebugView", 0, ini.c_str());
  Log("config: Enabled=%d DumpFrameKey=0x%02X DumpFrameAt=%d DumpShaders=%d Jitter=%d "
      "JitterDebugScale=%.2f",
      g_config.enabled, g_config.dump_frame_key, g_config.dump_frame_at, g_config.dump_shaders,
      g_config.jitter, g_config.jitter_debug_scale);
  Log("config: DLSS=%d Preset=%d JitterSign=%+.0f ToggleKey=0x%02X DebugView=%d", g_config.dlss,
      g_config.dlss_preset, g_config.dlss_jitter_sign, g_config.dlss_toggle_key,
      g_config.dlss_debug_view);
}

}  // namespace ds3
