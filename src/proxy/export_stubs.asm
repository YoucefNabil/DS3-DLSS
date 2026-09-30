; Pass-through exports: each stub tail-jumps into the real System32\d3d11.dll.
; g_real_procs is filled by dllmain.cpp in the same order as kForwardedNames.

EXTERN g_real_procs:QWORD

STUB MACRO name, index
PUBLIC stub_&name
stub_&name PROC
    jmp QWORD PTR [g_real_procs + index * 8]
stub_&name ENDP
ENDM

.code
STUB D3D11CreateDeviceForD3D12,               0
STUB D3DKMTCloseAdapter,                      1
STUB D3DKMTDestroyAllocation,                 2
STUB D3DKMTDestroyContext,                    3
STUB D3DKMTDestroyDevice,                     4
STUB D3DKMTDestroySynchronizationObject,      5
STUB D3DKMTPresent,                           6
STUB D3DKMTQueryAdapterInfo,                  7
STUB D3DKMTSetDisplayPrivateDriverFormat,     8
STUB D3DKMTSignalSynchronizationObject,       9
STUB D3DKMTUnlock,                            10
STUB D3DKMTWaitForSynchronizationObject,      11
STUB EnableFeatureLevelUpgrade,               12
STUB OpenAdapter10,                           13
STUB OpenAdapter10_2,                         14
STUB CreateDirect3D11DeviceFromDXGIDevice,    15
STUB CreateDirect3D11SurfaceFromDXGISurface,  16
STUB D3D11CoreCreateDevice,                   17
STUB D3D11CoreCreateLayeredDevice,            18
STUB D3D11CoreGetLayeredDeviceSize,           19
STUB D3D11CoreRegisterLayers,                 20
STUB D3D11On12CreateDevice,                   21
STUB D3DKMTCreateAllocation,                  22
STUB D3DKMTCreateContext,                     23
STUB D3DKMTCreateDevice,                      24
STUB D3DKMTCreateSynchronizationObject,       25
STUB D3DKMTEscape,                            26
STUB D3DKMTGetContextSchedulingPriority,      27
STUB D3DKMTGetDeviceState,                    28
STUB D3DKMTGetDisplayModeList,                29
STUB D3DKMTGetMultisampleMethodList,          30
STUB D3DKMTGetRuntimeData,                    31
STUB D3DKMTGetSharedPrimaryHandle,            32
STUB D3DKMTLock,                              33
STUB D3DKMTOpenAdapterFromHdc,                34
STUB D3DKMTOpenResource,                      35
STUB D3DKMTQueryAllocationResidency,          36
STUB D3DKMTQueryResourceInfo,                 37
STUB D3DKMTRender,                            38
STUB D3DKMTSetAllocationPriority,             39
STUB D3DKMTSetContextSchedulingPriority,      40
STUB D3DKMTSetDisplayMode,                    41
STUB D3DKMTSetGammaRamp,                      42
STUB D3DKMTSetVidPnSourceOwner,               43
STUB D3DKMTWaitForVerticalBlankEvent,         44
STUB D3DPerformance_BeginEvent,               45
STUB D3DPerformance_EndEvent,                 46
STUB D3DPerformance_GetStatus,                47
STUB D3DPerformance_SetMarker,                48

END
