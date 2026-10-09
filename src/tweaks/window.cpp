#include "tweaks/window.h"

#include <atomic>
#include <cwchar>

#include "common/config.h"
#include "core.h"
#include "tweaks/game_memory.h"

namespace ds3::window {
namespace {

constexpr DWORD kFrameStyle = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
constexpr DWORD kFrameExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;

std::atomic<HWND> g_window{nullptr};
// True while the window is borderless and kept on its monitor: the game's
// windowed mode, from its first ShowWindow on. False in full screen mode
// (DXGI places the window: see OnFullscreenState) and before the first show
// (left as the game made it).
std::atomic<bool> g_windowed{false};
std::atomic<bool> g_shown{false};
std::atomic<bool> g_got_size{false};  // a WM_SIZE reached the game since the first show began
WNDPROC g_game_wndproc = nullptr;

using SetWindowLongWFn = LONG(WINAPI*)(HWND, int, LONG);
using ShowWindowFn = BOOL(WINAPI*)(HWND, int);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
SetWindowLongWFn o_SetWindowLongW = nullptr;
ShowWindowFn o_ShowWindow = nullptr;
SetCursorPosFn o_SetCursorPos = nullptr;

bool HasFrame(DWORD style) { return BorderlessStyle(style) != style; }

bool MonitorRect(HMONITOR monitor, RECT* rect) {
  MONITORINFO info = {sizeof(info)};
  if (!GetMonitorInfoW(monitor, &info)) return false;
  *rect = info.rcMonitor;
  return true;
}

// Any move or resize of the windowed game becomes "cover the monitor" (the one
// it is being moved to, so Win+Shift+arrow still changes monitor).
void FillMonitor(HWND hwnd, WINDOWPOS* pos) {
  if ((pos->flags & SWP_NOMOVE) && (pos->flags & SWP_NOSIZE)) return;  // z-order / show only
  if (!(pos->flags & SWP_NOMOVE) && pos->x == -32000 && pos->y == -32000) return;  // minimising
  HMONITOR target = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  if (!(pos->flags & SWP_NOMOVE) && !(pos->flags & SWP_NOSIZE)) {
    const RECT wanted = {pos->x, pos->y, pos->x + pos->cx, pos->y + pos->cy};
    target = MonitorFromRect(&wanted, MONITOR_DEFAULTTONEAREST);
  }
  RECT rect = {};
  if (!MonitorRect(target, &rect)) return;
  pos->x = rect.left;
  pos->y = rect.top;
  pos->cx = rect.right - rect.left;
  pos->cy = rect.bottom - rect.top;
  pos->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_WINDOWPOSCHANGING && g_windowed)
    FillMonitor(hwnd, reinterpret_cast<WINDOWPOS*>(lparam));
  if (message == WM_SIZE) g_got_size = true;
  return CallWindowProcW(g_game_wndproc, hwnd, message, wparam, lparam);
}

// Takes the frame off (style and extended style), then a real move / resize to
// the monitor, which gives the game its WM_SIZE when the size changes.
void MakeBorderless(HWND hwnd, DWORD style) {
  g_windowed = true;
  o_SetWindowLongW(hwnd, GWL_EXSTYLE,
                   static_cast<LONG>(static_cast<DWORD>(GetWindowLongW(hwnd, GWL_EXSTYLE)) &
                                     ~kFrameExStyle));
  o_SetWindowLongW(hwnd, GWL_STYLE, static_cast<LONG>(BorderlessStyle(style)));
  RECT rect = {};
  if (!MonitorRect(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &rect))
    GetWindowRect(hwnd, &rect);  // keep its size; at least the frame is gone
  SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
               SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// The game's window: class FDPclass (title "FDP" at first, "DARK SOULS III"
// later). It is created exactly as the game asks: created frameless, it would
// get its first WM_SIZE inside CreateWindowExW, while the game still ignores
// it (see Hk_ShowWindow).
bool Adopt(HWND hwnd) {
  wchar_t class_name[64] = {}, title[64] = {};
  GetClassNameW(hwnd, class_name, 64);
  GetWindowTextW(hwnd, title, 64);
  const DWORD style = static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE));
  if (!IsGameWindow(class_name, title, style)) return false;
  g_window = hwnd;
  g_game_wndproc = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
  Log("window: game window \"%ls\" (%ls) style 0x%08lX: %p", title, class_name, style,
      static_cast<void*>(hwnd));
  return true;
}

// The game takes its UI size from the client area on WM_SIZE, but only once it
// has stored its window handle, just after CreateWindowExW; it then calls
// ShowWindow. The frame comes off here, before the window is visible: the
// resize is a real one, so the game gets a real WM_SIZE, and the first thing
// on screen is the borderless window. If the size happened not to change (no
// WM_SIZE), the game is sent one, as a framed window gets in its first show.
BOOL WINAPI Hk_ShowWindow(HWND hwnd, int command) {
  if (!hwnd || command == SW_HIDE || g_shown || (!g_window && !Adopt(hwnd)) || hwnd != g_window)
    return o_ShowWindow(hwnd, command);
  g_shown = true;
  const DWORD style = static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE));
  if (!HasFrame(style)) {
    Log("window: first show, full screen style 0x%08lX: unchanged", style);
    return o_ShowWindow(hwnd, command);
  }
  g_got_size = false;
  MakeBorderless(hwnd, style);
  const BOOL result = o_ShowWindow(hwnd, command);
  RECT client = {};
  GetClientRect(hwnd, &client);
  const bool got_size = g_got_size;
  if (!got_size)
    SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client.right, client.bottom));
  Log("window: first show, borderless %ldx%ld (WM_SIZE %s)", client.right, client.bottom,
      got_size ? "from the resize" : "sent: size unchanged");
  return result;
}

// The game switches between windowed and full screen through this.
LONG WINAPI Hk_SetWindowLongW(HWND hwnd, int index, LONG value) {
  if (!hwnd || hwnd != g_window || !g_shown) return o_SetWindowLongW(hwnd, index, value);
  if (index == GWL_STYLE) {
    const DWORD style = static_cast<DWORD>(value);
    const bool windowed = HasFrame(style);
    Log("window: game style 0x%08lX (%s)", style,
        windowed ? "windowed: borderless" : "full screen: unchanged");
    if (windowed) {
      const LONG previous = GetWindowLongW(hwnd, GWL_STYLE);
      MakeBorderless(hwnd, style);
      return previous;
    }
    g_windowed = false;
  } else if (index == GWL_EXSTYLE && g_windowed) {
    value = static_cast<LONG>(static_cast<DWORD>(value) & ~kFrameExStyle);
  }
  return o_SetWindowLongW(hwnd, index, value);
}

// The game re-centres the cursor even when it is in the background.
BOOL WINAPI Hk_SetCursorPos(int x, int y) {
  const HWND hwnd = g_window;
  if (hwnd && GetForegroundWindow() != hwnd) return TRUE;
  return o_SetCursorPos(x, y);
}

template <typename Fn>
bool Hook(uint8_t* base, const char* function, Fn hook, Fn* original) {
  const bool ok = game_memory::HookImport(base, "USER32.dll", function,
                                          reinterpret_cast<void*>(hook),
                                          reinterpret_cast<void**>(original));
  if (!ok) Log("window: import %s not found", function);
  return ok;
}

template <typename Fn>
void Unhook(uint8_t* base, const char* function, Fn original) {
  void* ignored = nullptr;
  if (original)
    game_memory::HookImport(base, "USER32.dll", function, reinterpret_cast<void*>(original),
                            &ignored);
}

}  // namespace

DWORD BorderlessStyle(DWORD style) {
  if ((style & WS_CHILD) || (style & WS_CAPTION) != WS_CAPTION) return style;
  return (style & ~kFrameStyle) | WS_POPUP;
}

bool IsGameWindow(LPCWSTR class_name, LPCWSTR title, DWORD style) {
  if (style & WS_CHILD) return false;
  if (class_name && !IS_INTRESOURCE(class_name) && _wcsicmp(class_name, L"FDPclass") == 0)
    return true;
  return title && std::wcsncmp(title, L"DARK SOULS", 10) == 0;
}

void OnFullscreenState(BOOL fullscreen) {
  // Leaving full screen needs nothing: the game then restyles the window
  // (SetWindowLongW), which makes it borderless again.
  if (fullscreen && g_windowed.exchange(false)) Log("window: exclusive full screen: unchanged");
}

void Apply() {
  if (!GetConfig().window_borderless) return;
  if (game_memory::Ds3DebugFpsInstalled()) {
    Log("window: DS3DebugFps is installed (D3DCompiler_43.dll + FPSconfig.ini), it has its own "
        "borderless mode: [Window] skipped. Remove those two files to use ours.");
    return;
  }
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  // All or nothing: without one of them the window would be left half done.
  if (!Hook(base, "SetWindowLongW", &Hk_SetWindowLongW, &o_SetWindowLongW) ||
      !Hook(base, "ShowWindow", &Hk_ShowWindow, &o_ShowWindow)) {
    Unhook(base, "SetWindowLongW", o_SetWindowLongW);
    Log("window: borderless not available");
    return;
  }
  Hook(base, "SetCursorPos", &Hk_SetCursorPos, &o_SetCursorPos);
  Log("window: borderless on");
}

}  // namespace ds3::window
