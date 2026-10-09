#pragma once
// [Window] Borderless: the game's windowed mode without a frame, covering its
// whole monitor (like DS3DebugFps: the image is stretched to the screen when
// the game resolution is smaller, e.g. 1600x900 on a 1920x1080 display).
//
// Done through the exe's own imports of ShowWindow and SetWindowLongW. The
// window is created exactly as the game asks; in its first ShowWindow, still hidden, it loses its frame and is resized to the monitor,
// a real resize, so the game gets the WM_SIZE it takes its UI size from (like
// DS3DebugFps, which does the same much later, on a visible window). When the
// game switches to windowed mode in its options (SetWindowLongW), the same
// happens. The window procedure (WM_WINDOWPOSCHANGING) keeps the window on
// the monitor's rectangle whatever position and size the game asks for.
//
// The game window is recognised by its class (FDPclass): it is created with
// the title "FDP" and only renamed "DARK SOULS III" later.
//
// Alt-tab: DS3DebugFps makes its window TOPMOST, so it stays over whatever you
// switch to. We never touch the z-order. The game also re-centres the mouse
// cursor every 0.5 s (exe+0xF13440 on v1.15.2) without checking that it has
// focus, which drags the cursor back into it after alt-tab; SetCursorPos calls
// from the exe are dropped while the game is not the foreground window.
#include <windows.h>

namespace ds3::window {

// Called from DllMain, before the game creates its window. Does nothing unless
// [Window] Borderless=1.
void Apply();

// From IDXGISwapChain::SetFullscreenState. The game's exclusive full screen
// mode never calls SetWindowLongW, so this is how the window stops being kept
// on the monitor while DXGI owns it.
void OnFullscreenState(BOOL fullscreen);

// --- exposed for tests ---

// style without caption / sizing frame, with WS_POPUP. Child windows and
// styles that have no caption (the game's full screen mode) are unchanged.
DWORD BorderlessStyle(DWORD style);

// True for the game's main window (class FDPclass, or a "DARK SOULS" title).
bool IsGameWindow(LPCWSTR class_name, LPCWSTR title, DWORD style);

}  // namespace ds3::window
