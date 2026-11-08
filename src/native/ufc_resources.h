// SvR 2008: host resources for the game's own D3D textures and surfaces.
//
// With the null GPU, SvR's D3D library creates real guest resources (headers + physical memory)
// itself; the native renderer gives each a host counterpart the first time it's used. Surfaces
// become host render targets; resolve destinations (including the front buffer) become host
// textures the GPU writes; any other texture is mirrored from guest memory (native_texture_mirror).

#pragma once

#include <rex/types.h>

namespace bd::gpu {

struct GuestTexture;
struct GuestShader;
struct GuestVertexDeclaration;

// Guest D3DSurface (render or depth target) -> host render target.
GuestTexture *SvrResolveSurface(u32 surface_va);
// Guest texture that the GPU writes (Resolve destination, front buffer) -> host texture. `source`
// = the surface a resolve copies from: a target made for it takes that surface's scale (NR-11b).
// defer_fill (NR-12a): a new target's fill from guest memory waits until something samples it
// (SvrFillSampledTargets, SvrResolveTexture) and leaves out what resolves wrote by then. The
// resolve that made it must then report itself: SvrPrepareTargetResolve before, and
// SvrNoteTargetResolved after a resolve that wrote.
GuestTexture *SvrResolveTarget(u32 texture_va, const GuestTexture *source = nullptr,
                               bool defer_fill = false);
// Before a resolve into a target: exact = the caller knows the host rect it will write (else the
// owed fill is done now, so the resolve lands on top of it).
void SvrPrepareTargetResolve(u32 texture_va, bool exact);
// After an exact resolve wrote host pixels [left, right) x [top, bottom) of `level`.
void SvrNoteTargetResolved(u32 texture_va, u32 level, i32 left, i32 top, i32 right, i32 bottom);
// Before a draw: fills any bound target whose fill is still owed (one atomic load when none is).
void SvrFillSampledTargets(const GuestTexture *const *slots, u32 count);
// Guest texture bound for sampling -> host texture (a resolve target, else a mirror).
GuestTexture *SvrResolveTexture(u32 texture_va);
// Mirrors over this physical range were made from bytes that have changed: the next bind rebuilds
// them. Any thread, no lock needed (NR-10b write-back, NR-10c encoder).
void SvrMirrorsInvalidateRange(u32 physical, u32 size);
// Called once per presented frame (mirror checks are per frame).
void SvrOnFrame();

// Internal resolution as a multiple of the guest's (1280x720): host render targets and resolve
// textures are this many times larger, and viewports/scissors are scaled to match. UFC:
// ufc3_native_scale (steps of 0.25), else the game's resolution_scale cvar (svr_render_scale > 0
// overrides that one; SvR's pick from the display is not used). Fixed for the session once the
// first render target exists.
// Guest pixels -> host pixels of `target` (null = the session's scale). Every edge of every rect
// goes through this one rounding (half up), so rects that share an edge still share it; a target
// that stays 1x (GuestTexture::keep1x) gets the guest value back.
i32 SvrScalePx(i32 guest_px, const GuestTexture *target);
// The same for a viewport edge, which the game stores as a float.
float SvrScaleEdge(float guest_px, const GuestTexture *target);
// The scale itself, for what isn't a coordinate (occlusion counts). 1 with ufc3_native_off bit 2048.
float SvrAreaScale();
// ufc3_native_off bit 2048: whole-number scale only, no 1x targets, as before NR-11.
bool SvrWholeScaleOnly();
// What svr_render_scale = 0 (Auto) picks on this PC: 1 on Steam Deck and built-in graphics,
// otherwise from the screen (2 at 1080p/1440p, 3 at 4K). Shown by the settings menu.
u32 SvrAutoRenderScale();
// One guest draw reached the host (drawn) or was dropped on the way (no target, no shader, no
// pipeline); SvrOnFrame logs both per frame every 600 frames.
void SvrNoteDraw(bool drawn);
// Frames presented so far, plus 1 (never 0); per-frame mirror checks compare against it.
u64 SvrFrameIndex();

// Shaders and vertex declarations stay the game's own D3D objects; each one the game creates is
// given a host counterpart (shared between objects with the same microcode / elements).
void SvrOnShaderCreated(u32 shader_va, u32 function_va, bool pixel);
GuestShader *SvrLookupShader(u32 shader_va);
void SvrOnVertexDeclarationCreated(u32 decl_va, u32 elements_va);
GuestVertexDeclaration *SvrLookupVertexDeclaration(u32 decl_va);

}  // namespace bd::gpu
