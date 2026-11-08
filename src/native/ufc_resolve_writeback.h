// NR-10b: resolves reach guest memory on the native renderer, when something needs them there.
//
// A resolve only copies between host textures, so its pixels never reach guest memory. Most nobody
// reads there. Three readers do: the CPU (pictures built from a render, a save), a texture bound
// through another header over the same memory (a mirror), and the C++ DXT5 encoder (NR-10c).
// Each resolve is remembered and its pages lose guest access, as readback_resolve=ondemand does on
// the emulated renderer (sdk_patches/0014). A reader gets the pixels first, on demand. Nothing is
// read back at resolve time.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <rex/types.h>

namespace bd::gpu {

struct GuestTexture;

// False with ufc3_native_off bit 4096: the renderer behaves as before NR-10b.
bool UfcWritebackOn();

// A resolve that reached `host` (hooks/draw.cpp). Guest pixels, as the game passed them: `have_rect`
// false = the whole surface. Clamped here the way Video::ResolveRegionToTexture clamps.
void UfcWritebackNoteResolve(u32 dest_va, GuestTexture *host, const GuestTexture *surface, u32 level,
                             u32 face, bool have_rect, i32 left, i32 top, i32 right, i32 bottom,
                             i32 dst_x, i32 dst_y);

// NR-14d2: whether a remembered resolve still needs `host`'s pixels. Rects none of whose pages are
// still hidden lost out to something newer (Serve would skip them) and are dropped here first.
bool UfcWritebackHolds(const GuestTexture *host);

// Whether a page of this physical range may hold a resolve not yet in guest memory. Lock-free, a
// few words of a bitmap: safe per bind per frame.
bool UfcResolvePending(u32 physical, size_t size);

// UfcResolveToGuest below, for a caller that already has the physical address.
// keep (NR-12a): leaves that host texture's own resolves pending.
void UfcResolveToGuestPhysical(u32 physical, size_t size, const char *why,
                               const GuestTexture *keep = nullptr);

// Host code writing guest memory through the physical view: gives read-only pages write access for
// the copy (par/10.md G5). Call UfcResolveToGuest on the range first, and invalidate the mirrors after.
void UfcWriteGuestPhysical(u32 physical, const void *data, size_t size);

// A pointer into the physical view (NativeMirrorSource::base, TranslatePhysical) -> physical address.
u32 UfcPhysicalOf(const u8 *physical_view_pointer);

// NR-10g: a host target just made for `texture_va` starts with what its guest memory holds (every
// level, pending resolves written back first), so a partial resolve into one level leaves the other
// levels as the hardware would: the CAF composite blends level N of a texture over itself and
// resolves the result back into level N, through a header made over memory an earlier pass resolved
// into. Guest -> host, the exact inverse of the write-back's texel path. 2D only; false = nothing
// uploaded (cube, depth, a format the write-back doesn't do, no device). Caller must not hold
// state().mutex. NR-12a: `levels` = bit per mip level to fill (the others are left as they are);
// `covered` = host-pixel rects a resolve already wrote since the target was made, left out.
struct UfcFillRect {
  u32 level;
  i32 left, top, right, bottom;
};
bool UfcFillTargetFromGuest(u32 texture_va, GuestTexture *host, u32 levels = ~0u,
                            const std::vector<UfcFillRect> *covered = nullptr);

}  // namespace bd::gpu

// Blocks until every remembered resolve touching the range is in guest memory. Flushes and waits for
// the GPU only if one is really outstanding. `guest_address`: a fetch constant's base_address << 12
// or a physical address, both work. Never call with state().mutex held. Any host code about to read
// or overwrite such memory through the physical view calls this first: it doesn't fault there.
void UfcResolveToGuest(uint32_t guest_address, size_t size);
