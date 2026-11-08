// Run-time gate for every native renderer hook (NR-8).
//
// SvR picks its renderer at compile time, so its hooks replace the game's functions outright. UFC
// carries both renderers in one exe (ufc3_renderer, ufc_native.h): with the default (emulated) a
// hooked function must run exactly the game's own body. Force-included into the native sources
// after ufc_d3d_names.h (cmake/native_renderer.cmake), so it redefines rex/hook.h's REX_HOOK and
// REX_HOOK_RAW before any source includes it (#pragma once keeps them). ufc_hook.h's
// SVR_HOOK_AFTER/BEFORE/RESULT check the same latch.
//
// The original body is a weak reference: re:Blue hooks a few names UFC doesn't map (they compile
// to unused functions, see ufc_d3d_names.h), and those have no __imp__ to call.

#pragma once

#include <rex/hook.h>

#include "ufc_native.h"

#define UFC_IMP_WEAK(subroutine) \
  extern "C" __attribute__((weak)) REX_FUNC(__imp__##subroutine)

#undef REX_HOOK
#define REX_HOOK(subroutine, function)                    \
  UFC_IMP_WEAK(subroutine);                               \
  extern "C" REX_FUNC(subroutine) {                       \
    if (!g_ufc3_native) {                                 \
      __imp__##subroutine(ctx, base);                     \
      return;                                             \
    }                                                     \
    rex::ppc::HostToGuestFunction<function>(ctx, base);   \
  }

#undef REX_HOOK_RAW
#define REX_HOOK_RAW(name)                                                          \
  static void ufc_native_##name(PPCContext& __restrict ctx, uint8_t* base);         \
  UFC_IMP_WEAK(name);                                                               \
  extern "C" REX_FUNC(name) {                                                       \
    if (!g_ufc3_native) {                                                           \
      __imp__##name(ctx, base);                                                     \
      return;                                                                       \
    }                                                                               \
    ufc_native_##name(ctx, base);                                                   \
  }                                                                                 \
  static void ufc_native_##name([[maybe_unused]] PPCContext& __restrict ctx, uint8_t* base)
