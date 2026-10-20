// ufc3 - persist the first-run answers.
//
// On boot the game runs a first-run dialog state machine (sub_82975280,
// this+8 = state, 0..22): replay-data notice, grappling-controls choice and the
// guided-tutorial question. The answers only change the in-memory system data
// (e.g. bit 0x20 of [sys+0x80] is cleared once the grappling choice is made).
// Nothing in that machine requests a system-data save, so SYSTEM.DAT keeps the
// first-run bits and every boot asks again. With bit 0x20 saved as clear, state
// 17 jumps straight to the end and skips the grappling and tutorial prompts.
// The replay-data notice (state 15) has no condition and shows on every boot.
//
// When the machine reaches its final state and the first-run flags changed, we
// issue the same system-data save (kind 0) the game builds elsewhere, e.g.
// sub_82933BF0 at 0x82933D6C:
//   params = {sys, ..., [0x6C]=7}; SaveParams(obj); obj.Init(params);
//   SaveWrapper::Request(wrapper, 0, &obj); obj.~SaveParams();

#pragma once

#include <atomic>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(ufc3_save_first_run, true, "ufc3",
                    "Save system data when the first-run prompts are finished");

REX_EXTERN(__imp__sub_82975280);  // first-run dialog state machine update
REX_EXTERN(sub_8234E5F0);         // save-params object constructor
REX_EXTERN(sub_82A22CE8);         // save-params object init from descriptor
REX_EXTERN(sub_82EE9128);         // save wrapper: request(kind, params)
REX_EXTERN(sub_82259088);         // save-params object destructor

namespace ufc3_first_run {

constexpr uint32_t kSaveWrapperPtr = 0x83574A00;   // global save wrapper
constexpr uint32_t kSystemDataPtr = 0x83577434;    // global system data
constexpr uint32_t kParamsVtable = 0x82011B1C;     // set before the destructor
constexpr uint32_t kFinalState = 22;
constexpr uint32_t kFlagsOffset = 0x80;            // first-run flag word in sys

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}

inline void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
  value = _byteswap_ulong(value);
  std::memcpy(base + addr, &value, 4);
}

inline void Call(PPCFunc* fn, const PPCContext& parent, uint8_t* base, uint32_t sp,
                 uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0) {
  PPCContext c = parent;
  c.r1.u64 = sp;
  c.r3.u64 = r3;
  c.r4.u64 = r4;
  c.r5.u64 = r5;
  fn(c, base);
}

inline void RequestSystemSave(PPCContext& ctx, uint8_t* base) {
  uint32_t wrapper = Load32(base, kSaveWrapperPtr);
  uint32_t sys = Load32(base, kSystemDataPtr);
  if (!wrapper || !sys) {
    REXLOG_WARN("first-run save skipped: wrapper={:08X} sys={:08X}", wrapper, sys);
    return;
  }
  // Same layout as the game's 0x24B0-byte frame: descriptor at sp+0x50,
  // params object at sp+0xD0.
  uint32_t sp = (ctx.r1.u32 - 0x2600) & ~0xFu;
  std::memset(base + sp + 0x48, 0, 0x88);
  Store32(base, sp + 0x50, sys);
  Store32(base, sp + 0xBC, 7);
  Call(sub_8234E5F0, ctx, base, sp, sp + 0xD0);
  Call(sub_82A22CE8, ctx, base, sp, sp + 0xD0, sp + 0x50);
  Call(sub_82EE9128, ctx, base, sp, wrapper, 0, sp + 0xD0);
  Store32(base, sp + 0xD0, kParamsVtable);
  Call(sub_82259088, ctx, base, sp, sp + 0xD0);
  REXLOG_INFO("first-run prompts finished; requested system data save");
}

}  // namespace ufc3_first_run

extern "C" REX_FUNC(sub_82975280) {
  using namespace ufc3_first_run;
  // Flag word as it was when the machine started (state 0).
  static std::atomic<uint32_t> start_flags{0};
  uint32_t self = ctx.r3.u32;
  uint32_t before = Load32(base, self + 8);
  uint32_t sys = Load32(base, kSystemDataPtr);
  if (before == 0 && sys) {
    start_flags = Load32(base, sys + kFlagsOffset);
  }
  __imp__sub_82975280(ctx, base);
  uint32_t after = Load32(base, self + 8);
  if (before == after) {
    return;
  }
  REXLOG_DEBUG("first-run dialog state {} -> {}", before, after);
  if (after == kFinalState && sys && REXCVAR_GET(ufc3_save_first_run)) {
    uint32_t flags = Load32(base, sys + kFlagsOffset);
    if (flags != start_flags) {
      REXLOG_INFO("first-run flags {:08X} -> {:08X}", start_flags.load(), flags);
      RequestSystemSave(ctx, base);
    }
  }
}
