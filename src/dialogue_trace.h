// ufc3 - trace Wwise dynamic dialogue (--ufc3_trace_dialogue).
//
// Bruce Buffer's ring intro and the commentary are Wwise dialogue events
// (lists in DAT/sound/ImtDialogEvent.dat) resolved to an audio node and queued
// on a dynamic sequence; the intro director (sub_82BAC5D8) advances whenever
// the item playing changes. To see what a stalled intro waits for, this logs:
//   - ResolveDialogueEvent (sub_82F18A78: r3 event, r4 argument values, r5
//     count) with the arguments and the node it returns (0 = no leaf);
//   - the dynamic-sequence enqueue (sub_82F1A4D8: r3 sequence, r4 node,
//     r6 event, r7 delay);
//   - the item a sequence is playing (sub_82F1AE28: r3 sequence), on change.

#pragma once

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "dialogue_poll_fix.h"

REXCVAR_DEFINE_BOOL(ufc3_trace_dialogue, false, "ufc3",
                    "Log Wwise dialogue resolves, dynamic-sequence enqueues and the item playing");

REX_EXTERN(__imp__sub_82F18A78);  // AK::SoundEngine::DynamicDialogue::ResolveDialogueEvent
REX_EXTERN(__imp__sub_82F1A4D8);  // dynamic sequence playlist enqueue
REX_EXTERN(__imp__sub_82F1AE28);  // dynamic sequence: item playing now
REX_EXTERN(__imp__sub_827D6E68);  // the intro directors' debounced "item playing now"

namespace ufc3_dialogue_trace {

constexpr uint32_t kCornerBase = 0x83541FE0;  // fight setup, +0 = fighter id * 100
constexpr uint32_t kCornerStride = 20712;

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}

inline std::mutex& Mutex() {
  static std::mutex m;
  return m;
}

// Last item reported per sequence, so polling only logs changes.
inline std::unordered_map<uint32_t, uint32_t>& Playing() {
  static std::unordered_map<uint32_t, uint32_t> m;
  return m;
}

// What sub_82F1AE28 returned last on this thread (for sub_827D6E68).
struct PollResult {
  bool called;
  uint32_t value;
};
inline PollResult& LastPoll() {
  static thread_local PollResult r{};
  return r;
}

// A line that resolved to nothing is never queued; note it on the sequence
// its caller would have used, so the intro directors still hear of it.
constexpr uint32_t kPrideResolve = 0x82C3659C;  // sub_82C36530 (the PRIDE announcer's lines)
constexpr uint32_t kPrideEnqueue = 0x82C366B4;

// ufc3_dialogue_no_skip: every caller; ufc3_pride_dead_lines: the PRIDE
// announcer's lines only.
inline bool Tracks(uint32_t lr, uint32_t pride_lr) {
  return REXCVAR_GET(ufc3_dialogue_no_skip) || (REXCVAR_GET(ufc3_pride_dead_lines) && lr == pride_lr);
}

inline void Unresolved(uint32_t lr, uint32_t owner, uint32_t event) {
  if (!Tracks(lr, kPrideResolve)) return;
  uint32_t seq = 0;
  if (lr == 0x827D7044) seq = owner + 44;    // sub_827D7010: queues on this+44
  if (lr == kPrideResolve) seq = owner;      // sub_82C36530: this or this+28 (both checked)
  if (seq) ufc3_dialogue_poll::Queued(seq, event, true);
}

}  // namespace ufc3_dialogue_trace

extern "C" REX_FUNC(sub_82F18A78) {
  namespace dt = ufc3_dialogue_trace;
  const uint32_t event = ctx.r3.u32, args = ctx.r4.u32, n = ctx.r5.u32, r6 = ctx.r6.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t owner = ctx.r29.u32;  // the caller's object (callee-saved)
  if (!REXCVAR_GET(ufc3_trace_dialogue)) {
    __imp__sub_82F18A78(ctx, base);
    if (ctx.r3.u32 == 0) dt::Unresolved(lr, owner, event);
    return;
  }
  // Saved LRs up the guest stack (back chain at [sp], caller's LR at [back - 8]).
  std::string chain;
  for (uint32_t sp = ctx.r1.u32, k = 0; k < 6; ++k) {
    uint32_t back = dt::Load32(base, sp);
    if (back <= sp || back - sp > 0x100000) break;
    chain += fmt::format(" {:08X}", dt::Load32(base, back - 8));
    sp = back;
  }
  std::string vals;
  for (uint32_t i = 0; i < n && i < 8 && args; ++i)
    vals += fmt::format("{}{:08X}", i ? " " : "", dt::Load32(base, args + 4 * i));
  __imp__sub_82F18A78(ctx, base);
  if (ctx.r3.u32 == 0) dt::Unresolved(lr, owner, event);
  REXLOG_INFO("[dlg] resolve {:08X} args[{}]=[{}] r6={:X} -> node {:08X} lr={:08X}{} fighters {} {}",
              event, n, vals, r6, ctx.r3.u32, lr, chain, dt::Load32(base, dt::kCornerBase) / 100,
              dt::Load32(base, dt::kCornerBase + dt::kCornerStride) / 100);
}

extern "C" REX_FUNC(sub_82F1A4D8) {
  namespace dt = ufc3_dialogue_trace;
  const uint32_t enq_lr = static_cast<uint32_t>(ctx.lr);
  if (REXCVAR_GET(ufc3_trace_dialogue)) {
    REXLOG_INFO("[dlg] enqueue seq={:08X} node={:08X} r5={:08X} event={:08X} delay={} r8={:X} r9={:X} lr={:08X}",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
                static_cast<uint32_t>(ctx.lr));
  }
  const uint32_t seq = ctx.r3.u32, event = ctx.r6.u32;
  __imp__sub_82F1A4D8(ctx, base);
  if (dt::Tracks(enq_lr, dt::kPrideEnqueue)) ufc3_dialogue_poll::Queued(seq, event, false);
  if (REXCVAR_GET(ufc3_trace_dialogue))
    REXLOG_INFO("[dlg]   enqueue -> {:X}", ctx.r3.u32);
}

extern "C" REX_FUNC(sub_82F1AE28) {
  namespace dt = ufc3_dialogue_trace;
  const uint32_t seq = ctx.r3.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82F1AE28(ctx, base);
  const bool full = REXCVAR_GET(ufc3_dialogue_no_skip);
  if (full || REXCVAR_GET(ufc3_pride_dead_lines)) {
    const uint32_t real = ctx.r3.u32;
    ctx.r3.u64 = ufc3_dialogue_poll::Poll(seq, real, !full);
    dt::LastPoll() = {true, ctx.r3.u32};
    if (REXCVAR_GET(ufc3_trace_dialogue) && ctx.r3.u32 != real)
      REXLOG_INFO("[dlg-fix] seq {:08X}: reporting {:08X} (playing {:08X})", seq, ctx.r3.u32, real);
  }
  if (!REXCVAR_GET(ufc3_trace_dialogue)) return;
  std::lock_guard lock(dt::Mutex());
  auto [it, fresh] = dt::Playing().try_emplace(seq, ctx.r3.u32);
  if (fresh || it->second != ctx.r3.u32) {
    it->second = ctx.r3.u32;
    REXLOG_INFO("[dlg] playing seq={:08X} -> {:08X} lr={:08X}", seq, ctx.r3.u32, lr);
  }
}

// sub_827D6E68(sound, debounced): with r4 != 0 it returns the line playing
// only once it has stayed current for 5 ticks. With the fix on, pass the
// (fixed) value on at once.
extern "C" REX_FUNC(sub_827D6E68) {
  namespace dt = ufc3_dialogue_trace;
  const bool debounced = ctx.r4.u32 != 0;
  dt::LastPoll().called = false;
  __imp__sub_827D6E68(ctx, base);
  if (REXCVAR_GET(ufc3_dialogue_no_skip) && debounced && dt::LastPoll().called)
    ctx.r3.u64 = dt::LastPoll().value;
}
