cmake_minimum_required(VERSION 3.21)
project(DS3_DLSS LANGUAGES C CXX ASM_MASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "DS3 is x64 only: configure with -A x64")
endif()

# Static CRT for the tests and tools too (the mod gets it from dlaa_core_add_mod).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared proxy / hooks / NGX (C:\_APPS\CLAUDE\dlaa-core).
set(DLAA_CORE_DIR ${CMAKE_SOURCE_DIR}/../dlaa-core CACHE PATH "dlaa-core checkout")
add_subdirectory(${DLAA_CORE_DIR} dlaa_core)

# Our shaders, precompiled with fxc into headers.
dlaa_core_compile_shader(src/dlss/mv_from_depth.hlsl cs_5_0 kMvFromDepthCs mv_from_depth.h)
dlaa_core_compile_shader(src/dlss/object_motion.hlsl ps_5_0 kObjectMotionPs object_motion_ps.h)

add_library(ds3_dlss SHARED
  src/game.cpp
  src/common/config.cpp
  src/camera/scene_camera.cpp
  src/dlss/dlaa.cpp
  src/dlss/object_motion.cpp
  src/dlss/reactive.cpp
  src/dlss/capture.cpp
  src/widescreen/widescreen.cpp
  src/tweaks/game_memory.cpp
  src/tweaks/frame_rate.cpp
  src/tweaks/window.cpp
)
dlaa_core_add_mod(ds3_dlss SHADERS mv_from_depth object_motion_ps)

# Standalone smoke test: loads the built proxy and drives device/swap-chain creation.
add_executable(smoke_test tests/smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE dxgi d3dcompiler)

# DLAA path test with a synthetic DS3-like frame (needs the game's dumped PS 393A947C35535471).
add_executable(dlaa_test tests/dlaa_test.cpp)
target_link_libraries(dlaa_test PRIVATE dxgi d3dcompiler)

# [Widescreen] table search and GraphicsConfig.xml rewrite.
add_executable(widescreen_test tests/widescreen_test.cpp src/widescreen/widescreen.cpp
  src/common/config.cpp)
target_include_directories(widescreen_test PRIVATE src)
dlaa_core_target_defaults(widescreen_test)
target_link_libraries(widescreen_test PRIVATE dlaa_core)

# [FrameRate] / [Window] code search on the real exe, sprint factors, window maths.
add_executable(tweaks_test tests/tweaks_test.cpp src/tweaks/game_memory.cpp
  src/tweaks/frame_rate.cpp src/tweaks/window.cpp src/common/config.cpp)
target_include_directories(tweaks_test PRIVATE src)
dlaa_core_target_defaults(tweaks_test)
target_link_libraries(tweaks_test PRIVATE dlaa_core)

# Constant-buffer listing of the dumped game shaders (camera-matrix audit).
add_executable(cb_scan tools/cb_scan.cpp)
target_link_libraries(cb_scan PRIVATE d3dcompiler dxguid)

# Offline replay of a [Debug] CaptureKey capture through DLSS, with metrics.
add_executable(capture_replay tools/capture_replay.cpp)
target_include_directories(capture_replay PRIVATE src)
dlaa_core_target_defaults(capture_replay)
target_link_libraries(capture_replay PRIVATE windowscodecs ole32)
