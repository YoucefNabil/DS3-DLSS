// [FrameRate] / [Window] test (no game needed).
//   tweaks_test [path\to\DarkSoulsIII.exe]
// - finds the sprint check, the flip object slot and the frame limiter in the
//   real exe (when installed), at the v1.15.2 addresses
// - sprint factors: same as the game at 60 fps, same check in time at any rate
// - borderless window end to end: hooks this exe's own USER32 imports, then
//   creates / restyles / resizes a "DARK SOULS III" window the way the game
//   does. Runs from a copy in %TEMP%\ds3_tweaks_test with its own ini.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "common/config.h"
#include "common/log.h"
#include "tweaks/frame_rate.h"
#include "tweaks/game_memory.h"
#include "tweaks/window.h"

using namespace ds3;

static int g_failures = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failures;
}
static bool Near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

// One sprint-check step as the game does it. Returns the new multiplier.
static float Step(const frame_rate::SprintFactors& f, float speed, float seconds, float multiplier) {
  const bool stuck = speed * seconds * f.speed_scale < 1.0f;
  if (stuck) return multiplier > 0.5f ? multiplier * f.slowdown : 0.5f;
  return multiplier < 1.0f ? multiplier * f.recovery : 1.0f;
}

static void SprintFactors() {
  const frame_rate::SprintFactors stock = frame_rate::ForFrameTime(0.0f);
  Check(stock.speed_scale == 30.0f && stock.slowdown == 0.8f && stock.recovery == 1.2f,
        "no frame time: the game's own constants");
  const frame_rate::SprintFactors at60 = frame_rate::ForFrameTime(1.0f / 60.0f);
  Check(Near(at60.speed_scale, 30.0f, 1e-3f) && Near(at60.slowdown, 0.8f) &&
            Near(at60.recovery, 1.2f),
        "60 fps: same as the game");

  // A sprint start at 2.7 units/s: fine at 60 fps, "stuck" at 144 fps in the
  // stock check (the bug), fine again with the fix.
  const float rate = 144.0f, dt = 1.0f / rate;
  Check(Step(stock, 2.7f, 1.0f / 60.0f, 1.0f) == 1.0f, "stock 60 fps: 2.7 u/s not stuck");
  Check(Step(stock, 2.7f, dt, 1.0f) < 1.0f, "stock 144 fps: 2.7 u/s slowed (the bug)");
  const frame_rate::SprintFactors at144 = frame_rate::ForFrameTime(dt);
  Check(Step(at144, 2.7f, dt, 1.0f) == 1.0f, "fixed 144 fps: 2.7 u/s not stuck");
  Check(Step(at144, 1.9f, dt, 1.0f) < 1.0f, "fixed 144 fps: really stuck (1.9 u/s) still caught");

  // Same speed change per second as 60 fps: 1/60 s of slowdown at 144 fps.
  for (const float fps : {30.0f, 120.0f, 144.0f, 240.0f, 360.0f}) {
    const frame_rate::SprintFactors f = frame_rate::ForFrameTime(1.0f / fps);
    const float per_60th_slow = std::pow(f.slowdown, fps / 60.0f);
    const float per_60th_rec = std::pow(f.recovery, fps / 60.0f);
    const float threshold = 1.0f / (f.speed_scale / fps);  // units/s below which "stuck"
    char what[96];
    std::snprintf(what, sizeof(what), "%.0f fps: 0.8 / 1.2 per 1/60 s, threshold 2 u/s (%.3f)",
                  fps, threshold);
    Check(Near(per_60th_slow, 0.8f, 1e-3f) && Near(per_60th_rec, 1.2f, 1e-3f) &&
              Near(threshold, 2.0f, 1e-3f),
          what);
  }
  const frame_rate::SprintFactors hitch = frame_rate::ForFrameTime(2.0f);
  Check(hitch.slowdown > 0.4f, "a 2 s hitch is clamped (slowdown stays sane)");
}

static void RealExe(const wchar_t* path) {
  HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!module) {
    std::printf("skip  real exe (%ls not found)\n", path);
    return;
  }
  auto* base = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module) & ~uintptr_t{3});
  const game_memory::Range code = game_memory::CodeSection(base);
  Check(code.begin != nullptr, "real exe: .text found");

  const frame_rate::SprintSite site = frame_rate::FindSprintCheck(code);
  std::printf("      sprint check at exe+0x%llX\n",
              static_cast<unsigned long long>(site.function ? site.function - base : 0));
  Check(site.function != nullptr, "real exe: sprint check found, constants 30 / 0.8 / 1.2");
  if (site.function && code.end - code.begin == 0x2719e00)  // v1.15.2
    Check(site.function - base == 0x5C72B0, "real exe: at the v1.15.2 address");

  uint8_t** slot = frame_rate::FindFlipperSlot(code);
  std::printf("      flip object slot at exe+0x%llX\n",
              static_cast<unsigned long long>(slot ? reinterpret_cast<uint8_t*>(slot) - base : 0));
  Check(slot != nullptr, "real exe: flip object slot found");
  if (slot && code.end - code.begin == 0x2719e00)
    Check(reinterpret_cast<uint8_t*>(slot) - base == 0x489DD10, "real exe: at the v1.15.2 address");
  Check(frame_rate::HasDebugLimiter(code), "real exe: limiter reads +0x354 / +0x358");

  void* page = game_memory::AllocateNear(base, 4096);
  Check(page && std::llabs(static_cast<uint8_t*>(page) - base) < 0x7FFF0000LL,
        "memory allocated within 2 GB of the exe");
  if (page) VirtualFree(page, 0, MEM_RELEASE);
  FreeLibrary(module);
}

static void Redirect() {
  // mulss xmm0, [rip + disp] in a buffer, redirected to a float next to it.
  static uint8_t code[64] = {0xF3, 0x0F, 0x59, 0x05, 0, 0, 0, 0};
  float* value = reinterpret_cast<float*>(code + 32);
  *value = 7.0f;
  Check(game_memory::Redirect(code + 4, value) && game_memory::RipTarget(code + 4) ==
                                                       reinterpret_cast<uint8_t*>(value),
        "Redirect points the operand at the new value");
}

static void WindowMaths() {
  Check(window::BorderlessStyle(WS_OVERLAPPEDWINDOW | WS_VISIBLE) == (WS_POPUP | WS_VISIBLE),
        "windowed style -> popup");
  Check(window::BorderlessStyle(0x00CA0000) == WS_POPUP, "game's windowed style 0xCA0000 -> popup");
  Check(window::BorderlessStyle(0x80000000) == 0x80000000, "full screen style unchanged");
  Check(window::BorderlessStyle(WS_CHILD | WS_CAPTION) == (WS_CHILD | WS_CAPTION), "child unchanged");
  Check(window::IsGameWindow(L"FDPclass", L"FDP", 0x00CA0000), "game window: class FDPclass");
  Check(window::IsGameWindow(L"Other", L"DARK SOULS III", 0x00CA0000), "game window: title");
  Check(!window::IsGameWindow(L"FDPclass", L"FDP", WS_CHILD), "child is not the game window");
  Check(!window::IsGameWindow(MAKEINTRESOURCEW(0xC123), L"FDP", 0), "class atom, other title: no");
  Check(!window::IsGameWindow(L"Other", nullptr, 0), "other window: no");
}

// Window and client area both exactly the monitor.
static bool FillsMonitor(HWND hwnd) {
  RECT rect = {}, client = {};
  GetWindowRect(hwnd, &rect);
  GetClientRect(hwnd, &client);
  MONITORINFO mi = {sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
  const RECT& m = mi.rcMonitor;
  std::printf("      window %ldx%ld at %ld,%ld, client %ldx%ld, monitor %ldx%ld at %ld,%ld\n",
              rect.right - rect.left, rect.bottom - rect.top, rect.left, rect.top, client.right,
              client.bottom, m.right - m.left, m.bottom - m.top, m.left, m.top);
  return EqualRect(&rect, &m) && client.right == m.right - m.left &&
         client.bottom == m.bottom - m.top;
}

// WM_SIZE as the game sees it: it takes its UI size from the client area then.
static int g_size_messages = 0;
static LPARAM g_last_size = 0;
static LRESULT CALLBACK TestWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_SIZE) {
    ++g_size_messages;
    g_last_size = lparam;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

// Runs in the temp copy, with [Window] Borderless=1 in its ini.
static void WindowEndToEnd() {
  SetSelfModule(GetModuleHandleW(nullptr));
  LogInit();
  LoadConfig();
  window::Apply();

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = TestWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"FDPclass";  // the game's class
  RegisterClassExW(&wc);

  // Like the game: AdjustWindowRect for the render size, then CreateWindowExW.
  const int w = 800, h = 450;
  RECT rect = {0, 0, w, h};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  Check(rect.right - rect.left > w && rect.bottom - rect.top > h,
        "AdjustWindowRect left to the game (frame added)");
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"FDP", WS_OVERLAPPEDWINDOW, 0, 0,
                              rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                              wc.hInstance, nullptr);
  Check(hwnd != nullptr, "window created");
  if (!hwnd) return;
  Check((GetWindowLongW(hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION,
        "created as the game asks (framed, until its first show)");
  // The game ignores a WM_SIZE that comes before it has stored the handle, i.e.
  // during CreateWindowExW; it needs one in (or after) the first ShowWindow.
  ShowWindow(hwnd, SW_HIDE);
  Check((GetWindowLongW(hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION,
        "SW_HIDE is not the first show");
  const int sizes_before_show = g_size_messages;
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  MONITORINFO mi = {sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
  Check(g_size_messages > sizes_before_show &&
            LOWORD(g_last_size) == mi.rcMonitor.right - mi.rcMonitor.left &&
            HIWORD(g_last_size) == mi.rcMonitor.bottom - mi.rcMonitor.top,
        "first ShowWindow: WM_SIZE with the monitor's size (the game's UI size)");
  const DWORD style = static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE));
  Check((style & WS_CAPTION) != WS_CAPTION && (style & WS_POPUP), "first show: no frame");
  Check(FillsMonitor(hwnd), "first show: covers the monitor");
  Check(!(GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST), "not topmost");

  // The game re-applies its windowed style, then resizes (menu resolution change).
  SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
  Check((GetWindowLongW(hwnd, GWL_STYLE) & WS_CAPTION) != WS_CAPTION, "restyle stays borderless");
  RECT bigger = {0, 0, 1024, 576};
  AdjustWindowRect(&bigger, WS_OVERLAPPEDWINDOW, FALSE);
  SetWindowPos(hwnd, nullptr, 50, 60, bigger.right - bigger.left, bigger.bottom - bigger.top,
               SWP_NOZORDER | SWP_NOACTIVATE);
  Check(FillsMonitor(hwnd), "a resize still covers the monitor");
  SetWindowPos(hwnd, nullptr, 5, 5, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  Check(FillsMonitor(hwnd), "a move still covers the monitor");

  // Cursor re-centring while the game window is in the background is dropped
  // (the test window was shown without activating it).
  POINT before = {}, after = {};
  GetCursorPos(&before);
  if (GetForegroundWindow() != hwnd) {
    SetCursorPos(before.x + 37, before.y + 23);
    GetCursorPos(&after);
    Check(after.x == before.x && after.y == before.y, "SetCursorPos ignored in the background");
  } else {
    std::printf("skip  cursor check (the test window has focus)\n");
  }

  // Full screen mode: the game's popup style is left alone and not moved.
  SetWindowLongW(hwnd, GWL_STYLE, static_cast<LONG>(WS_POPUP | WS_VISIBLE));
  SetWindowPos(hwnd, nullptr, 3, 4, 640, 360, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  RECT full = {};
  GetWindowRect(hwnd, &full);
  Check(full.left == 3 && full.top == 4, "full screen style: position left to the game");

  // Back to windowed (the game's style switch, with a frame in the ex style).
  SetWindowLongW(hwnd, GWL_EXSTYLE, WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
  SetWindowLongW(hwnd, GWL_STYLE, 0x00CA0000 | WS_VISIBLE);
  Check((GetWindowLongW(hwnd, GWL_STYLE) & WS_CAPTION) != WS_CAPTION &&
            !(GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_CLIENTEDGE),
        "full screen -> windowed: borderless");
  Check(FillsMonitor(hwnd), "full screen -> windowed: covers the monitor");

  // Exclusive full screen: the game only calls SetFullscreenState (no restyle),
  // and DXGI places the window.
  window::OnFullscreenState(TRUE);
  SetWindowPos(hwnd, nullptr, 7, 8, 640, 360, SWP_NOZORDER | SWP_NOACTIVATE);
  GetWindowRect(hwnd, &full);
  Check(full.left == 7 && full.top == 8 && full.right - full.left == 640,
        "exclusive full screen: window left to DXGI");
  window::OnFullscreenState(FALSE);
  SetWindowLongW(hwnd, GWL_STYLE, 0x00CA0000 | WS_VISIBLE);
  Check(FillsMonitor(hwnd), "exclusive -> windowed: covers the monitor again");
  DestroyWindow(hwnd);
}

int wmain(int argc, wchar_t** argv) {
  if (argc > 1 && std::wstring(argv[1]) == L"--window") {
    WindowEndToEnd();
    return g_failures;
  }
  SprintFactors();
  Redirect();
  WindowMaths();
  RealExe(argc > 1 ? argv[1]
                   : L"G:\\SteamLibrary\\steamapps\\common\\DARK SOULS III\\Game\\DarkSoulsIII.exe");

  // Window end to end in a temp copy (its own ini; never next to a real one).
  wchar_t temp[MAX_PATH] = {}, self[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, temp);
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  const std::wstring dir = std::wstring(temp) + L"ds3_tweaks_test\\";
  CreateDirectoryW(dir.c_str(), nullptr);
  const std::wstring exe = dir + L"tweaks_test.exe";
  const std::wstring ini = dir + L"DS3_DLSS.ini";
  CopyFileW(self, exe.c_str(), FALSE);
  WritePrivateProfileStringW(L"Window", L"Borderless", L"1", ini.c_str());
  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi = {};
  std::wstring command = L"\"" + exe + L"\" --window";
  std::fflush(stdout);
  if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(),
                     &si, &pi)) {
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    g_failures += static_cast<int>(code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  } else {
    Check(false, "window end-to-end process started");
  }

  std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures,
              g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
