cmake_minimum_required(VERSION 3.21)
project(DS3_DLSS LANGUAGES C CXX ASM_MASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "DS3 is x64 only: configure with -A x64")
endif()

# Static CRT so the mod has no redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(external/minhook EXCLUDE_FROM_ALL)

# NVIDIA DLSS SDK (external/dlss): NGX headers + static-CRT import library.
set(DLSS_SDK_DIR ${CMAKE_SOURCE_DIR}/external/dlss)
set(NGX_LIB_DIR ${DLSS_SDK_DIR}/lib/Windows_x86_64/x64)

# Motion-vector compute shader, precompiled with fxc into a header.
file(GLOB FXC_DIRS LIST_DIRECTORIES true "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*/x64")
list(SORT FXC_DIRS COMPARE NATURAL ORDER DESCENDING)
find_program(FXC_EXECUTABLE fxc HINTS ${FXC_DIRS} REQUIRED)
set(SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
  OUTPUT ${SHADER_OUT_DIR}/mv_from_depth.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT_DIR}
  COMMAND ${FXC_EXECUTABLE} /nologo /T cs_5_0 /E main /O3 /Vn kMvFromDepthCs
          /Fh ${SHADER_OUT_DIR}/mv_from_depth.h ${CMAKE_SOURCE_DIR}/src/dlss/mv_from_depth.hlsl
  DEPENDS ${CMAKE_SOURCE_DIR}/src/dlss/mv_from_depth.hlsl
  COMMENT "fxc mv_from_depth.hlsl"
  VERBATIM)

add_library(ds3_dlss SHARED
  src/proxy/dllmain.cpp
  src/proxy/export_stubs.asm
  src/proxy/exports.def
  src/common/log.cpp
  src/common/config.cpp
  src/hooks/vtable_indices.c
  src/hooks/d3d11_hooks.cpp
  src/hooks/frame_dump.cpp
  src/hooks/shader_registry.cpp
  src/camera/scene_camera.cpp
  src/dlss/dlaa.cpp
  ${SHADER_OUT_DIR}/mv_from_depth.h
)

set_target_properties(ds3_dlss PROPERTIES OUTPUT_NAME "d3d11")
target_include_directories(ds3_dlss PRIVATE src ${SHADER_OUT_DIR})
target_include_directories(ds3_dlss SYSTEM PRIVATE ${DLSS_SDK_DIR}/include)
target_compile_definitions(ds3_dlss PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(ds3_dlss PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/W4 /permissive- /utf-8>)
# SDK headers use nameless unions in their C-mode COM declarations (vtable_indices.c).
set_source_files_properties(src/hooks/vtable_indices.c PROPERTIES COMPILE_OPTIONS /wd4201)
target_link_libraries(ds3_dlss PRIVATE minhook dxguid
  ${NGX_LIB_DIR}/nvsdk_ngx_s$<$<CONFIG:Debug>:_dbg>.lib)

# Standalone smoke test: loads the built proxy and drives device/swap-chain creation.
add_executable(smoke_test tests/smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE dxgi d3dcompiler)


# DLAA path test with a synthetic DS3-like frame (needs the game's dumped PS 393A947C35535471).
add_executable(dlaa_test tests/dlaa_test.cpp)
target_link_libraries(dlaa_test PRIVATE dxgi d3dcompiler)
