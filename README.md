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
                                                      # (every mode also checks the mip-bias sampler swap)
dlaa_test <d3d11.dll> <393A947C35535471.ps.cso> mv     # with DebugView=1: motion vectors vs analytic values
dlaa_test <d3d11.dll> <393A947C35535471.ps.cso> edge   # slanted edge vs 16x16 supersampled reference
dlaa_test <d3d11.dll> <393A947C35535471.ps.cso> object # with DebugView=1: a moving quad gets object motion vectors;
                                                      # per-view cbMatrixWVP jittered like cbSceneParam
dlaa_test <d3d11.dll> <393A947C35535471.ps.cso> effect # with DebugView=2: reactive mask on an additive quad only
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
- `[DLSS] DebugView=1` draws the motion vectors over the final frame (red/green = horizontal/vertical
  speed, 1.0 = 4 px per frame;
  full blue = object motion from the game's velocity pass, dim blue = sky). The scene is not anti-aliased meanwhile.
- `DS3_DLSS.log` should show `DLSS feature created`, the scene depth line and the matrix self-check.
- `[DLSS] ObjectMotion=1` (default) adds per-object motion for characters and enemies, reusing the
  vertex shaders of the game's motion-blur velocity pass. Particles and effects still only get camera motion.
  **Turn the in-game Motion Blur option on**, otherwise the game skips that pass. `[DLSS] HideMotionBlur=1`
  (default) skips the blur draws (PS `E484BFEBC00C5CF7`) so the image is not blurred.
- `[DLSS] Reactive=1` (default) builds a reactive mask from the game's transparent effect draws
  (glows, spells, fire, particles): where they make up most of a pixel, DLSS favours the current frame
  over its history, so effects leave no trails. `0` turns it off. `[DLSS] DebugView=2` shows the mask
  (white = effect).
- `[DLSS] Sharpness=0` (default off, range 0-1) runs AMD's RCAS sharpening on the DLAA output, before
  tone mapping. 0.3-0.5 is a good start. DLSS's own sharpening was removed by NVIDIA in DLSS 2.5.1, so
  it is done here. Turn off driver sharpening (NVIDIA Image Scaling) so the two do not stack.
- `[DLSS] MipBias=-0.5` (default) samples textures from sharper mips in the 3D scene while DLAA runs
  (armour, cloth, ground). `-1` is NVIDIA's value for DLAA; lower values can shimmer on fine patterns
  like chainmail. `0` turns it off. **End** toggles it for comparison (`[DLSS] MipBiasKey`). Shadow,
  point and single-mip samplers are left alone, and post-processing and the UI keep the game's samplers.
- The jitter is applied to both copies of the camera the game uploads: cbSceneParam (b8) and
  the forward pass's cbMatrixWVP (b0). With only the first, objects drawn in the forward pass with a depth
  pre-pass (the player fading out during a bonfire warp) flickered.

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
- `tools/cb_scan.cpp`: lists every cbuffer variable of a shader dump (camera matrix audit)
- `tests/smoke_test.cpp`: drives the proxy outside the game
