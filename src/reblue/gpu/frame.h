/**
 * @file    gpu/frame.h
 * @brief   Shared by the TUs that record one frame: the command list ring,
 *          the draw path, the EDRAM resolve emulation, and present.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/device.h"

namespace bd::gpu {

void BeginCommandList(VideoState &s);

void DrainSlot(VideoState &s, u32 slot);

void AdvanceAndWaitReused(VideoState &s);

void SubmitOpenListLocked(VideoState &s);

// NR-10b: submits what is recorded, waits for it and reopens the same slot, so what the list copied
// to a readback buffer can be read now. Later draws re-issue everything: BeginCommandList dirties
// streams, pipeline and constants, and clears draw_framebuffer_bound, whose rebind dirties viewport
// and scissor. False if the list could not be reopened.
bool SvrFlushAndWaitLocked(VideoState &s);

// NR-14c: once `interval_us` of recording has passed since the list was opened, hands what it holds
// to the GPU with no fence and no wait, and goes on in a fresh list of the same slot, so a write-back
// flush later in the frame waits only for what was recorded since. At most kMaxListSplits a frame;
// the slot's next fenced submit covers the split lists (one queue, in order). Same re-issue as
// SvrFlushAndWaitLocked. Call between draws, never with an occlusion count open.
inline constexpr u32 kMaxListSplits = 8;
void SvrSplitListIfDueLocked(VideoState &s, i32 interval_us);
// For the write-back log: splits in the recording slot's frame so far, ms since its list was opened.
u32 SvrListSplits();
double SvrListOpenMs();

void ResolveEffectiveTargets(VideoState &s, GuestTexture *&rt,
                             GuestTexture *&ds);

plume::RenderFramebuffer *GetFramebuffer(VideoState &s, GuestTexture *rt,
                                         GuestTexture *ds);

// UFC: a resolve of part of a surface (D3DDevice_Resolve's pSourceRect / pDestPoint), in host
// pixels: `src` of the source goes to (dst_x, dst_y) of the destination, 1:1.
struct ResolveRegion {
  plume::RenderRect src;
  i32 dst_x = 0;
  i32 dst_y = 0;
};
bool CopySurfaceToTextureLocked(VideoState &s, GuestTexture *src,
                                GuestTexture *dst, const char *reason,
                                const ResolveRegion *region = nullptr);
bool MaterializeOutboundLocked(VideoState &s, GuestTexture *source,
                               bool aliasable_only = false);
void MaterializeInboundLocked(VideoState &s, GuestTexture *dst);
void DetachSourceSurfaceLocked(VideoState &s, GuestTexture *texture);
bool FullscreenChainClassLocked(const VideoState &s, const GuestTexture *t);

extern Video::OverlayDrawHook g_overlay_draw_hook;

} // namespace bd::gpu
