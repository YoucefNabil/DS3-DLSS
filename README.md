# DS3_DLSS

Native DLSS/DLAA for Dark Souls III (DX11), delivered as a `d3d11.dll` proxy.

**Status: Phase 3 (DLAA), ready for offline testing.** The proxy forwards all 51
`d3d11.dll` exports and hooks device/context/swap-chain calls. It jitters the main
camera through the game's own `cbSceneParam` upload (no game memory writes), builds
camera motion vectors from depth, and runs DLSS at native resolution (DLAA) on the HDR
scene right before the first post-process pass. It needs an RTX GPU. Upscaling modes are
not done yet.

## Build
```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\smoke_test.exe "%CD%\build\Release\d3d11.dll"   # expects "Present: 0x00000000" and "OK"
```
Clone with submodules (`git submodule update --init`).
Requirements: VS 2022 Build Tools, the Windows 10/11 SDK (for `fxc`), and the NVIDIA DLSS SDK
(`-DDLSS_SDK_DIR=...`; see `CMakeLists.txt` for the default path).

### DLAA test (no game needed)
`dlaa_test` simulates DS3's frame (cbSceneParam, depth clear, PS `393A947C35535471`) at 640x360.
It needs the game's pixel shader bytecode (from `[Debug] DumpShaders=1`), plus `nvngx_dlss.dll`
and a `DS3_DLSS.ini` next to `d3d11.dll` (with `[Debug] Enabled=1` for the DebugView modes):
```
dlaa_test <d3d11.dll> <393A947C35535471.ps.cso>        # grey scene stays grey, game bindings survive
                                                      # (every mode also checks the mip-bias sampler swap)
dlaa_test <d3d11.dll> <393A947C35535471.ps.cso> mv     # with DebugView=1: motion vectors vs analytic values
dlaa_test <d3d11.dll> <393A947C35535471.ps.cso> edge   # slanted edge vs 16x16 supersampled reference
dlaa_test <d3d11.dll> <393A947C35535471.ps.cso> object # with DebugView=1: a moving quad gets object motion vectors;
                                                      # per-view cbMatrixWVP jittered like cbSceneParam
dlaa_test <d3d11.dll> <393A947C35535471.ps.cso> effect # with DebugView=2: reactive mask on an additive quad only
dlaa_test <d3d11.dll> <393A947C35535471.ps.cso> ghost  # with DebugView=0: textured quad moving over a textured
                                                      # background: no smear inside it, no trail behind it
dlaa_test <d3d11.dll> <393A947C35535471.ps.cso> check  # with DebugView=3: the same scene shows no vector errors
```
Set `DS3_DLAA_TEST_DEBUG=1` to run with the D3D11 debug layer.

### Capture and replay (real game frames)
With `[Debug] Enabled=1`, press **F9** in game (`[Debug] CaptureKey`) to save the next 48 frames
(`CaptureFrames`) of everything DLSS gets and gives back, into `DS3_DLSS_capture_<date>_<time>\` next
to the DLL: scene colour, depth, per-object motion, effects, the motion vectors and masks, the output
and each frame's camera, jitter and settings (DDS + text; about 95 MB per frame at 1080p).
Stand still where the problem shows; hold the camera still for the static metrics.
`capture_replay` then runs the same frames through DLSS with the proxy's own code
(motion vectors included, so changes to `mv_from_depth.hlsl` can be tried on them too):
```
capture_replay <capture folder>                           # input, captured, DLSS
capture_replay <capture folder> DLSS --set DLSS.MipBias=-1 --crop 800,400,160,90 --zoom 6
capture_replay <capture folder> DLSS --save-dds              # every output frame as DDS
capture_replay <capture folder> DLSS --set Debug.Enabled=1 --set Debug.DebugView=3  # debug views as frames
```
Which pixels count as still is decided from depth and the recorded cameras, not from the proxy's
own motion vectors, so a motion vector bug cannot hide itself.
It prints a table (flicker and per-pixel std dev on still pixels and on fine detail, error against
the jitter-averaged input, sharpness of the output, motion and character stability) and writes per run
`metrics.txt`, `last.png`, `flicker.png` (temporal noise heat map) and zoomed crops. The replayed
motion vectors are checked against the game's (`mv 0.0000 px` = the replay matches the game).

## Install (testing)
Copy `build\Release\d3d11.dll` and `DS3_DLSS.ini` next to `DarkSoulsIII.exe`.
Uninstall by deleting both files. `Enabled=0` in the ini turns it into a pure pass-through.

It doesn't touch `dinput8.dll` (Blue Sentinel), `SeamlessCoop\`, or any game file.
It only writes `DS3_DLSS.log`, `DS3_DLSS_frame_*.txt` and (optionally) `DS3_DLSS_shaders` in the Game folder.
The optional `[Widescreen]`, `[FrameRate]` and `[Window]` sections (off by default) are the exception: see below.
NGX writes its own logs there too; its lines are also mirrored into `DS3_DLSS.log`.

## Using DLAA
- In-game: set **Anti-aliasing Off**. DLAA replaces it (the game's AA is a post-tonemap blur).
- **Home** toggles DLAA on/off for comparison (`[DLSS] ToggleKey`). It is the only hotkey that
  works by default.
- `[Debug] Enabled=0` (default) turns every debugging feature off, whatever its own setting:
  the F10 frame dump, F9 capture, End mip-bias toggle, shader dumps and DebugView. Set it to 1 to
  use them.
- `[Debug] DebugView=1` draws the motion vectors over the final frame (red/green = horizontal/vertical
  speed, 1.0 = 4 px per frame;
  full blue = object motion from the game's velocity pass, dim blue = sky). The scene is not anti-aliased meanwhile.
- `DS3_DLSS.log` should show `DLSS feature created`, the scene depth line and the matrix self-check.
- `[DLSS] ObjectMotion=1` (default) adds per-object motion for characters and enemies, reusing the
  vertex shaders of the game's motion-blur velocity pass. Particles and effects still only get camera motion.
  **Turn the in-game Motion Blur option on**, otherwise the game skips that pass. The game's motion
  blur then shows as usual.
- `[DLSS] Reactive=1` (default) builds a reactive mask from the game's transparent effect draws
  (glows, spells, fire, particles): where they make up most of a pixel, DLSS favours the current frame
  over its history, so effects leave no trails. `0` turns it off. `[Debug] DebugView=2` shows the mask
  (white = effect).
- `[Debug] DebugView=3` checks the motion vectors: last frame is moved along them and compared with this
  frame. Correct vectors stay dark; **red** marks pixels whose vector is wrong (these blur and smear in
  motion). A thin red edge behind moving things is normal (just uncovered).
  Anti-aliasing is off meanwhile.
- `[DLSS] MipBias=-0.5` (default) samples textures from sharper mips in the 3D scene while DLAA runs
  (armour, cloth, ground). `-1` is NVIDIA's value for DLAA; lower values can shimmer on fine patterns
  like chainmail. `0` turns it off. **End** toggles it for comparison (`[Debug] MipBiasKey`, needs `[Debug] Enabled=1`). Shadow,
  point and single-mip samplers are left alone, and post-processing and the UI keep the game's samplers.
- The jitter is applied to both copies of the camera the game uploads: cbSceneParam (b8) and
  the forward pass's cbMatrixWVP (b0). With only the first, objects drawn in the forward pass with a depth
  pre-pass (the player fading out during a bonfire warp) flickered.

## Widescreen (any resolution)
`[Widescreen] Enabled=1` lets the game render at any size and aspect ratio (ultrawide, or a phone stream
such as 2376x1080). The game only offers sixteen-by-nine sizes from a fixed table in the exe's `.data`; the
mod replaces its first entry (800x450) with the wanted size at start-up, before any game code runs. That
8-byte data write is the only change to game memory, and it is skipped when the size is in the table already.
- `Width=0 Height=0` (default) = the display's current resolution at each start, so a stream's virtual
  display is followed automatically. Or set a fixed size.
- `FollowDisplay=1` (default) also writes that size into `%APPDATA%\DarkSoulsIII\GraphicsConfig.xml`
  (windowed and full-screen resolution) at each start, so no menu visit is needed. `0` = pick it in the menu.
  A size picked in game during a session is not kept: the next start uses the display again.
- Play in windowed mode (`[Window] Borderless=1` works). The HUD is not changed.
- Changing the resolution in the game's menu mid-session is not handled: the game builds its 3D buffers
  at start-up, so the picture ends up in a corner. Restart the game instead (it opens at the display's
  size by itself).
- The log shows `widescreen:` lines (target size, table address, config rewrite). `tests/widescreen_test.cpp`
  checks the table search against the installed exe and the config rewrite.

## Frame rate unlock and sprint fix
`[FrameRate] Unlock=1` replaces the 60 fps cap with `FPS` (default 120). It uses the game's own debug frame
limiter, like DS3DebugFps: the flip object (pointer at exe+0x489DD10 on v1.15.2) gets its target rate at +0x354
and the flag at +0x358 set. With that flag the game also presents without vsync, so use a variable refresh rate
display or set `FPS` to the refresh rate.

With `FPS` above 80 the mod also fixes sprinting slowing down above ~82 fps (automatic, no ini key; at 80 or
below the bug does not happen and no game code is changed). The cause is the function at
exe+0x5C72B0: while sprinting, it treats the player as stuck when they moved less than 1/30 of a unit in one
frame (2 units/s at 60 fps), and then scales the movement speed by 0.8 per frame (down to 0.5), or by 1.2 per
frame back up to 1. The game passes it the frame time as its second argument, but the function never reads it.
The mod hooks it and redirects those three constants to values computed from that frame time
(`0.5 / dt`, `0.8 ^ (60 dt)`, `1.2 ^ (60 dt)`), so the check acts in time exactly as it does at 60 fps.
DS3DebugFps estimates the frame time from the flip object instead and adds fudge factors (x1.2, x1.5 above
90 fps); with the real frame time they are not needed.

While DS3DebugFps is installed (`D3DCompiler_43.dll` + `FPSconfig.ini` next to the exe), `[FrameRate]` and
`[Window]` stand down, since both mods would patch the same places.

## Borderless window
`[Window] Borderless=1` makes the game's windowed mode a frameless window covering its monitor, like
DS3DebugFps (a lower game resolution is stretched to the screen). Full-screen mode is left alone, and switching
between the two in game works.

The game takes its UI size from the client area on `WM_SIZE` (exe+0xEF50D0), but ignores it until it has stored
its window handle, just after `CreateWindowExW`; it then calls `ShowWindow`. So the window is created exactly as
the game asks, and the frame comes off in that first `ShowWindow`, while the window is still hidden: the style
loses its frame bits and the window is resized to the monitor. That is a real resize, so the game gets a real
`WM_SIZE` (only if the size did not change is one sent), and the first thing on screen is the borderless window.
A window created frameless instead gets its first `WM_SIZE` inside `CreateWindowExW`, too early, and the UI stays
at its 1280x720 default in the top-left corner. DS3DebugFps avoids that by removing the frame much later, on a
visible window. When the game switches to windowed mode (`SetWindowLongW`) the same happens, and
`WM_WINDOWPOSCHANGING` keeps the window on the rectangle of the monitor it is moved to. The exclusive full-screen
mode never restyles the window, so the swapchain's `SetFullscreenState(TRUE)` is what turns that off and leaves
the window to DXGI. The game window is recognised in its first `ShowWindow` by its class `FDPclass` (it is
created as "FDP" and renamed "DARK SOULS III" later).

The game has a hidden "fake full screen" mode of its own (a `WS_POPUP` window the size of the screen, debug
setting `isFakeFullScreen`), but it is chosen from settings read by Arxan-protected code and the menu would show
it as full screen, so it is not used.

Alt-tab: DS3DebugFps marks its window `TOPMOST`, so it stays above whatever you switch to. This mod never
touches the z-order. The game also re-centres the mouse cursor every 0.5 s (exe+0xF13440) when it is *not* the
foreground window, which keeps dragging the cursor back after alt-tab; the mod drops the exe's `SetCursorPos`
calls while another window has focus.

`tests/tweaks_test.cpp` finds the sprint check, flip object slot and limiter in the installed exe, checks the
sprint maths at 30-360 fps, and runs the borderless window hooks end to end on a test window.

## Phase 1: mapping the frame
1. Play **offline**. Stand somewhere with visible shimmer (e.g. Firelink Shrine).
2. With `[Debug] Enabled=1`, press **F10**. `DS3_DLSS_frame_<time>.txt` lists every draw/dispatch/copy/clear
   in the frame with VS/PS/CS hashes, render targets, depth, viewport, and the SRVs each pass reads.
   Resource ids (`T12`) are described at the end of the file. Work recorded on deferred
   contexts is spliced in (lines starting with `|`) under the `ExecuteCommandList` that runs it.
   With `[Debug] DumpShaders=1`, every shader the game creates is also saved to
   `DS3_DLSS_shaders\<hash>.<vs|ps|cs>.cso` for disassembly (set it before launching).
3. Take one dump with in-game **Anti-aliasing On** and one with it **Off**, and one with
   **Motion Blur On/Off**. Diffing them reveals the AA pass and any velocity buffer.
4. Make a RenderDoc capture of the same spot for pixel-level inspection.

## Layout
- `src/proxy/`: export table (`exports.def`, `export_stubs.asm`) and device-creation interception
- `src/hooks/`: MinHook vtable hooks, shader-hash registry, frame dump
- `src/common/`: log + ini
- `src/camera/`: cbSceneParam tracking and jitter
- `src/dlss/`: DLAA (NGX D3D11) and `mv_from_depth.hlsl`
- `src/widescreen/`: `[Widescreen]` resolution table entry, config follow
- `src/tweaks/`: `[FrameRate]` unlock + sprint fix, `[Window]` borderless
- `tests/dlaa_test.cpp`: DLAA pipeline tests outside the game
- `tools/cb_scan.cpp`: lists every cbuffer variable of a shader dump (camera matrix audit)
- `tools/capture_replay.cpp`: runs a `[Debug] CaptureKey` capture through DLSS, with metrics
- `tests/smoke_test.cpp`: drives the proxy outside the game
