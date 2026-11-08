// UFC 3 addresses for the XDK D3D functions that re:Blue's renderer hooks by name.
//
// Force-included into the native renderer sources (cmake/native_renderer.cmake), so
// REX_HOOK(D3DDevice_Clear, ...) overrides UFC's recompiled sub_8238E7B8 and
// __imp__D3DDevice_Clear calls its original body. The map is FINDINGS-nr1-d3d-map.md (matched by
// call shape and the device fields each function writes; "med"/"low" there = checked at run time by
// the NR-6 census, see FINDINGS-native-renderer.md "NR-6 census"). Functions not listed here are
// either not used by UFC's engine or not identified yet; a hook that names one fails to compile on
// purpose.
//
// UFC already hooks some D3D-range functions for --ufc3_cs60_trace (src/cs60_trace.h, generated):
// the internal resolver sub_8238C3A0 and the viewport setter sub_82377CF0 among them. Never map a
// hook onto one of those; hook the public function above it.

#pragma once

// Device / frame
#define D3DDevice_Present                     sub_8237C3E8
#define D3DDevice_Swap                        sub_8237BD60
#define D3DDevice_SynchronizeToPresentationInterval sub_8237B930
#define D3DDevice_Resolve                     sub_8238D4F0
#define D3DDevice_Clear                       sub_8238E7B8
#define D3DDevice_ClearF                      sub_8238E680
#define D3DDevice_SetRenderTarget             sub_82378088
#define D3DDevice_SetDepthStencilSurface      sub_82378418
#define D3DDevice_SetScissorRect              sub_82377610
#define D3DDevice_SetViewport                 sub_82377F58
#define D3DDevice_SetViewportF                sub_82377FD8

// Draws. UFC links D3DDevice_DrawVertices (SvR 2009/2010 don't) and BeginIndexedVertices.
#define D3DDevice_BeginVertices               sub_823832E8
#define D3DDevice_BeginIndexedVertices        sub_823837A8
#define D3DDevice_EndVertices                 sub_823832D8
#define D3DDevice_DrawVertices                sub_82383CE8
#define D3DDevice_DrawIndexedVertices         sub_82384100

// Buffers: the game creates, locks and unlocks its own; the renderer only observes Unlock.
// D3DDevice_CreateVertexBuffer sub_8236B478, D3DDevice_CreateIndexBuffer sub_8236B550,
// D3DVertexBuffer_Lock sub_8236C5B8 and D3DIndexBuffer_Lock sub_8236C608 are matched but
// deliberately NOT mapped: gpu/hooks/resource.cpp's REX_HOOKs would replace them.
#define D3DVertexBuffer_Unlock                sub_8236B540
#define D3DIndexBuffer_Unlock                 sub_8236B600

// Resource creation
#define D3DDevice_CreateVertexDeclaration     sub_82371CC0
#define D3DDevice_CreateVertexShader          sub_823721B8
#define D3DDevice_CreatePixelShader           sub_82371FD0

// State
#define D3DDevice_SetTexture                  sub_8236E9D8
#define D3DDevice_SetVertexShader             sub_823719A0
#define D3DDevice_SetPixelShader              sub_82371798
#define D3DDevice_SetVertexDeclaration        sub_82371BB8
#define D3DDevice_SetStreamSource             sub_82377758
#define D3DDevice_SetIndices                  sub_82377900
#define D3DDevice_SetVertexShaderConstantF    sub_82371440
#define D3DDevice_SetPixelShaderConstantF     sub_82371518

// Recorded work (NR-1 §3; the NR-6 census measures how much of a frame goes through them).
#define D3DDevice_BeginCommandBuffer          sub_82372A00
#define D3DDevice_EndCommandBuffer            sub_82373BF0
#define D3DDevice_RunCommandBuffer            sub_82374010
#define D3DDevice_BeginZPass                  sub_82380778
#define D3DDevice_EndZPass                    sub_82380F28
#define D3DDevice_BeginTiling                 sub_82380408
#define D3DDevice_EndTiling                   sub_82380A70
#define D3DQuery_Issue                        sub_82379540
#define D3DQuery_GetData                      sub_823797F0
// NR-6b: emits the 24-point pipe-flush buffer the device built at init (3 calls a fight frame).
#define D3DDevice_SetShaderGPRAllocation      sub_82371D40

// Device fields the census and hooks read (ufc_d3d.h has the full layout).
#define UFC3_D3D_DEVICE_RECORDING_CB          0x35E0  // BeginCommandBuffer stores the buffer here

// Original bodies (__imp__ = the recompiled function, for hooks that call through).
#define __imp__D3DDevice_Present              __imp__sub_8237C3E8
#define __imp__D3DDevice_Swap                 __imp__sub_8237BD60
#define __imp__D3DDevice_Resolve              __imp__sub_8238D4F0
#define __imp__D3DDevice_Clear                __imp__sub_8238E7B8
#define __imp__D3DDevice_ClearF               __imp__sub_8238E680
#define __imp__D3DDevice_SetTexture           __imp__sub_8236E9D8
#define __imp__D3DDevice_SetRenderTarget      __imp__sub_82378088
#define __imp__D3DDevice_SetDepthStencilSurface __imp__sub_82378418
#define __imp__D3DDevice_SetScissorRect       __imp__sub_82377610
#define __imp__D3DDevice_SetViewport          __imp__sub_82377F58
#define __imp__D3DDevice_SetViewportF         __imp__sub_82377FD8
#define __imp__D3DDevice_BeginVertices        __imp__sub_823832E8
#define __imp__D3DDevice_BeginIndexedVertices __imp__sub_823837A8
#define __imp__D3DDevice_EndVertices          __imp__sub_823832D8
#define __imp__D3DDevice_DrawVertices         __imp__sub_82383CE8
#define __imp__D3DDevice_DrawIndexedVertices  __imp__sub_82384100
#define __imp__D3DDevice_CreateVertexDeclaration __imp__sub_82371CC0
#define __imp__D3DDevice_CreateVertexShader   __imp__sub_823721B8
#define __imp__D3DDevice_CreatePixelShader    __imp__sub_82371FD0
#define __imp__D3DDevice_SetVertexShader      __imp__sub_823719A0
#define __imp__D3DDevice_SetPixelShader       __imp__sub_82371798
#define __imp__D3DDevice_SetVertexDeclaration __imp__sub_82371BB8
#define __imp__D3DDevice_SetStreamSource      __imp__sub_82377758
#define __imp__D3DDevice_SetIndices           __imp__sub_82377900
#define __imp__D3DDevice_SetVertexShaderConstantF __imp__sub_82371440
#define __imp__D3DDevice_SetPixelShaderConstantF  __imp__sub_82371518
#define __imp__D3DVertexBuffer_Unlock         __imp__sub_8236B540
#define __imp__D3DIndexBuffer_Unlock          __imp__sub_8236B600
#define __imp__D3DDevice_BeginCommandBuffer   __imp__sub_82372A00
#define __imp__D3DDevice_EndCommandBuffer     __imp__sub_82373BF0
#define __imp__D3DDevice_RunCommandBuffer     __imp__sub_82374010
#define __imp__D3DDevice_BeginZPass           __imp__sub_82380778
#define __imp__D3DDevice_EndZPass             __imp__sub_82380F28
#define __imp__D3DDevice_BeginTiling          __imp__sub_82380408
#define __imp__D3DDevice_EndTiling            __imp__sub_82380A70
#define __imp__D3DQuery_Issue                 __imp__sub_82379540
#define __imp__D3DQuery_GetData               __imp__sub_823797F0
#define __imp__D3DDevice_SetShaderGPRAllocation __imp__sub_82371D40

// Resource description getters (NR-1 med/low; the re:Blue hooks only need them to link until NR-8).
#define __imp__D3DSurface_GetDesc             __imp__sub_8236E8B0
#define __imp__D3DTexture_GetLevelDesc        __imp__sub_8236D590
#define __imp__D3DVolumeTexture_GetLevelDesc  __imp__sub_8236D690
