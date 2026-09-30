# DS3_DLSS

Native DLSS/DLAA for Dark Souls III (DX11), delivered as a `d3d11.dll` proxy.

**Status: Phase 3 (DLAA), ready for offline testing.** The proxy forwards all 51
`d3d11.dll` exports and hooks device/context/swap-chain calls. It jitters the main
camera through the game's own `cbSceneParam` upload (no game memory writes), builds
camera motion vectors from depth, and runs DLSS at native resolution (DLAA) on the HDR
scene right before the first post-process pass. Upscaling modes are not done yet.

## Build
```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\smoke_test.exe "%CD%\build\Release\d3d11.dll"   # expects "Present: 0x00000000" and "OK"
```
Requirements: VS 2022 Build Tools, the Windows 10/11 SDK (for `fxc`), and the NVIDIA DLSS SDK
(`-DDLSS_SDK_DIR=...`; see `CMakeLists.txt` for the default path).

### DLAA test (no game needed)
`dlaa_test` simulates DS3's frame (cbSceneParam, depth clear, PS `393A947C35535471`) at 640x360.
It needs the game's pixel shader bytecode (from `[Debug] DumpShaders=1`), plus `nvngx_dlss.dll`
and a `DS3_DLSS.ini` next to `d3d11.dll`:
```
dlaa_test <d3d11.dll> <393A947C35535471.ps.cso>        # grey scene stays grey, game bindings survive
dlaa_test <d3d11.dll> <393A947C35535471.ps.cso> mv     # with DebugView=1: motion vectors vs analytic values
dlaa_test <d3d11.dll> <393A947C35535471.ps.cso> edge   # slanted edge vs 16x16 supersampled reference
```
Set `DS3_DLAA_TEST_DEBUG=1` to run with the D3D11 debug layer.

## Install (testing)
Copy `build\Release\d3d11.dll` and `DS3_DLSS.ini` next to `DarkSoulsIII.exe`.
Uninstall by deleting both files. `Enabled=0` in the ini turns it into a pure pass-through.

It doesn't touch `dinput8.dll` (Blue Sentinel), `SeamlessCoop\`, or any game file.
It only writes `DS3_DLSS.log`, `DS3_DLSS_frame_*.txt` and (optionally) `DS3_DLSS_shaders` in the Game folder.
NGX writes its own logs there too; its lines are also mirrored into `DS3_DLSS.log`.

## Using DLAA
- In-game: set **Anti-aliasing Off**. DLAA replaces it (the game's AA is a post-tonemap blur).
- **Home** toggles DLAA on/off for comparison (`[DLSS] ToggleKey`).
- `[DLSS] DebugView=1` shows motion vectors (red/green = horizontal/vertical speed, blue = sky).
- `DS3_DLSS.log` should show `DLSS feature created`, the scene depth line and the matrix self-check.
- The motion vectors only cover camera motion, so moving characters and effects may ghost slightly.

## Phase 1: mapping the frame
1. Play **offline**. Stand somewhere with visible shimmer (e.g. Firelink Shrine).
2. Press **F10**. `DS3_DLSS_frame_<time>.txt` lists every draw/dispatch/copy/clear
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
- `tests/dlaa_test.cpp`: DLAA pipeline tests outside the game
- `tests/smoke_test.cpp`: drives the proxy outside the game
