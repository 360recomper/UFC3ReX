// Times the game's own wait for the null GPU: the block-on-fence body of UFC's D3D (sub_8236F328,
// FINDINGS-nr1-d3d-map.md, under BlockOnFence sub_8236F3F8 and the ring's KickOff sub_82370460).
// Nothing is drawn by that GPU, but the game still waits for it, so a command processor that falls
// behind costs frame time on the game's thread.

#include <cstdint>
#include <windows.h>

#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/hook.h>

#include "ufc_native.h"
#include "ufc_frame_diag.h"

REXCVAR_DECLARE(int32_t, ufc3_native_off);
REX_EXTERN(__imp__sub_8236F328);
REX_EXTERN(__imp__sub_82374768);

namespace {
// NR-P6d: set while the game's Swap waits on its frame throttle (the call at 0x8237C360).
thread_local bool t_swap_wait = false;
}  // namespace

REX_HOOK_RAW(sub_8236F328) {
  bd::gpu::SvrDiagTimer wait(bd::gpu::SvrDiag::kGuestGpuWait);
  bd::gpu::SvrZone zone(bd::gpu::SvrZoneId::kGuestWait);
  const bool outer = t_swap_wait;
  t_swap_wait = g_ufc3_native && static_cast<uint32_t>(ctx.lr) == 0x8237C364 &&
                !(REXCVAR_GET(ufc3_native_off) & 524288);
  __imp__sub_8236F328(ctx, base);
  t_swap_wait = outer;
}

// The poll inside sub_8236F328's wait loop: the game spins it with no yield until the fence passes. On Swap's
// throttle the host thread sleeps 1 ms after each poll that says "keep waiting". The game's own order is kept
// (KickOff before the wait, its fence test after each poll, its hang check by guest ticks), so frame order and
// the guest's vblank pacing are unchanged; only the core stops burning.
REX_HOOK_RAW(sub_82374768) {
  __imp__sub_82374768(ctx, base);
  if (t_swap_wait && ctx.r3.u32 != 0) {
    bd::gpu::SvrZone sleep_zone(bd::gpu::SvrZoneId::kGuestSleep);
    Sleep(1);
  }
}
