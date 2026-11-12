// ufc3 - the game's physical memory: the tale-of-the-tape crash fix (--ufc3_physical_spill, on by
// default) and a trace (--ufc3_trace_physical). See FINDINGS-tot-crash.md.
//
// Spill: a game heap whose physical window is full gets its allocation placed in free physical memory
// above the window instead of failing (details at TopOfWindows below).
//
// Trace: every physical allocation and free of 256 KB or more with the caller and the running total,
// every failed one, a per-game-heap summary every 20 s (live + run peak), and at the first failure of a
// burst (after the boot-time memory probe) the live allocations grouped by game heap and guest
// back-trace plus the parent physical heap map.
//
// Guest wrappers (all callers go through them):
//   sub_823C75B0  XPhysicalAlloc(size, ulPhysicalAddress, alignment, protect)
//   sub_823C7638  XPhysicalAllocEx(size, low, high, alignment, protect)
//   sub_823C76B0  XPhysicalFree(address)  (tail call to MmFreePhysicalMemory)
// The game's physical allocator sub_82284EB8(size, flags) calls XPhysicalAllocEx (lr 0x82284F7C)
// with r25 = the game heap id (sub_8279C6C8 / the flags' type table) and low/high = that heap's
// physical window (64-byte table at 0x8335A8C0, +4 low, +8 high); its own caller's LR is saved at
// sp+416-8 of its frame, which is r1 in our hook.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "heap_trace.h"  // ufc3_heap_trace::HeapInfo (game heap names)

REXCVAR_DEFINE_BOOL(ufc3_trace_physical, false, "ufc3",
                    "Log the game's physical allocations (XPhysicalAlloc/Free), failures with the "
                    "live allocations by game heap and caller, and the physical heap map");

REXCVAR_DEFINE_BOOL(ufc3_physical_spill, true, "ufc3",
                    "When a game heap's physical window is full, place the allocation in free physical "
                    "memory above it (tale-of-the-tape crash fix, FINDINGS-tot-crash.md); false = the "
                    "original behaviour (the allocation fails)");

REX_EXTERN(__imp__sub_823C75B0);
REX_EXTERN(__imp__sub_823C7638);
REX_EXTERN(__imp__sub_823C76B0);

namespace ufc3_phys_trace {

constexpr uint32_t kGameAllocLr = 0x82284F7C;  // sub_82284EB8's call to XPhysicalAllocEx
constexpr uint32_t kGameAllocFrame = 416;

struct Block {
  uint32_t size;
  uint32_t lr;      // caller of the wrapper
  uint32_t caller;  // caller of sub_82284EB8 (0 = other path)
  int32_t heap;     // game heap id (-1 = other path)
  uint32_t low, high;
  uint32_t bt[4];   // guest back-trace above sub_82284EB8's caller (LRs from the back-chain)
};

// Per game heap: live and peak bytes over the run.
struct HeapUse {
  uint64_t live = 0, peak = 0;
  double peak_at = 0;
};

struct State {
  std::mutex m;
  std::unordered_map<uint32_t, Block> live;  // guest VA -> block
  std::map<int32_t, HeapUse> heaps;
  uint64_t live_bytes = 0;
  uint32_t failures = 0;
  uint32_t dumps = 0;
  std::chrono::steady_clock::time_point first{}, last_dump{}, last_summary{};
  bool started = false;
};

inline State& S() {
  static State s;
  return s;
}

inline double Mb(uint64_t b) { return b / 1048576.0; }

inline double Since(const State& s) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - s.first).count();
}

// Live bytes per game heap / window. Caller holds s.m.
inline void SummaryLocked(State& s, uint8_t* base, const char* tag) {
  std::map<std::tuple<int32_t, uint32_t, uint32_t>, std::pair<uint64_t, uint32_t>> by_heap;
  for (auto& [va, b] : s.live) {
    auto& e = by_heap[{b.heap, b.low, b.high}];
    e.first += b.size;
    ++e.second;
  }
  REXLOG_INFO("[phys] {} at {:.1f} s: live {:.2f} MB in {} blocks", tag, Since(s), Mb(s.live_bytes),
              s.live.size());
  for (auto& [k, v] : by_heap) {
    auto [heap, low, high] = k;
    const HeapUse& u = s.heaps[heap];
    REXLOG_INFO("[phys]   heap {} window {:08X}-{:08X} ({:.2f} MB): {:.2f} MB in {} blocks, run peak {:.2f} MB at "
                "{:.1f} s{}{}", heap, low, high, Mb(uint64_t(high) - low + 1), Mb(v.first), v.second, Mb(u.peak),
                u.peak_at, heap >= 0 ? " | " : "",
                heap >= 0 ? ufc3_heap_trace::HeapInfo(base, uint32_t(heap)) : std::string());
  }
}

// Caller holds s.m.
inline void DumpLocked(State& s, uint8_t* base, uint32_t size, int32_t heap) {
  REXLOG_WARN("[phys] DUMP at {:.1f} s: failed {} bytes in heap {}", Since(s), size, heap);
  SummaryLocked(s, base, "live at the failure");
  using Key = std::tuple<int32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
  std::map<Key, std::pair<uint64_t, uint32_t>> by_caller;
  for (auto& [va, b] : s.live) {
    auto& e = by_caller[{b.heap, b.caller ? b.caller : b.lr, b.bt[0], b.bt[1], b.bt[2], b.bt[3]}];
    e.first += b.size;
    ++e.second;
  }
  std::vector<std::pair<Key, std::pair<uint64_t, uint32_t>>> v(by_caller.begin(), by_caller.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
  for (size_t i = 0; i < v.size() && i < 60; ++i) {
    auto [heap, c, b0, b1, b2, b3] = v[i].first;
    REXLOG_WARN("[phys]   heap {} caller {:08X} < {:08X} < {:08X} < {:08X} < {:08X}: {:.2f} MB in {} blocks", heap,
                c, b0, b1, b2, b3, Mb(v[i].second.first), v[i].second.second);
  }
  auto* mem = REX_KERNEL_MEMORY();
  if (auto* ph = mem ? mem->GetPhysicalHeap() : nullptr) {
    REXLOG_WARN("[phys] parent heap: {} of {} pages unreserved", ph->GetUnreservedPageCount(),
                ph->GetTotalPageCount());
    if (s.dumps == 1)
      ph->DumpMap();
  }
}

inline void OnAlloc(uint8_t* base, uint32_t va, const Block& blk, const char* what, uint32_t align) {
  State& s = S();
  std::lock_guard lk(s.m);
  auto now = std::chrono::steady_clock::now();
  if (!s.started) {
    s.started = true;
    s.first = s.last_summary = now;
  }
  if (va) {
    s.live[va] = blk;
    s.live_bytes += blk.size;
    HeapUse& u = s.heaps[blk.heap];
    u.live += blk.size;
    if (u.live > u.peak) {
      u.peak = u.live;
      u.peak_at = Since(s);
    }
    if (blk.size >= 256 * 1024)
      REXLOG_INFO("[phys] {:.1f} s: {} {:.2f} MB align {:X} heap {} caller {:08X} < {:08X} < {:08X} < {:08X} -> {:08X} "
                  "(live {:.2f} MB, heap {:.2f} MB)",
                  Since(s), what, Mb(blk.size), align, blk.heap, blk.caller ? blk.caller : blk.lr, blk.bt[0],
                  blk.bt[1], blk.bt[2], va, Mb(s.live_bytes), Mb(u.live));
    if (now - s.last_summary > std::chrono::seconds(20)) {
      s.last_summary = now;
      SummaryLocked(s, base, "summary");
    }
    return;
  }
  ++s.failures;
  if (Since(s) < 20.0)
    return;  // the boot-time memory probe (harmless, ~245 failures in the first 0.1 s)
  REXLOG_WARN("[phys] {:.1f} s: {} {} bytes align {:X} heap {} window {:08X}-{:08X} caller {:08X} FAILED "
              "(live {:.2f} MB)",
              Since(s), what, blk.size, align, blk.heap, blk.low, blk.high, blk.caller ? blk.caller : blk.lr,
              Mb(s.live_bytes));
  if (s.dumps < 3 && (s.dumps == 0 || now - s.last_dump > std::chrono::seconds(10))) {
    ++s.dumps;
    s.last_dump = now;
    DumpLocked(s, base, blk.size, blk.heap);
  }
}

inline void OnFree(uint32_t va, uint32_t lr) {
  State& s = S();
  std::lock_guard lk(s.m);
  auto it = s.live.find(va);
  if (it == s.live.end())
    return;
  s.live_bytes -= it->second.size;
  s.heaps[it->second.heap].live -= it->second.size;
  if (it->second.size >= 256 * 1024)
    REXLOG_INFO("[phys] {:.1f} s: free {:.2f} MB at {:08X} (heap {} caller {:08X}, free lr {:08X}, live {:.2f} MB)",
                Since(s), Mb(it->second.size), va, it->second.heap,
                it->second.caller ? it->second.caller : it->second.lr, lr, Mb(s.live_bytes));
  s.live.erase(it);
}

// ---- spill (the fix) ----
// The game splits physical memory into one window per game heap (64-byte entries at 0x8335A8C0, +4 low,
// +8 high, count at 0x8335A8A4) and sub_82284EB8 asks XPhysicalAllocEx for [low, high] only; it then
// rejects an address below low, nothing else. In a match, heap 25 (the scene/cutscene textures) has
// 41 MB and sits under heap 27 (47 MB, the top of the layout). Skipping a walkout part with A loads the
// next scene's textures while the skipped one's are still resident: heap 25 peaks at 39-41 MB with ~1 MB
// to spare, and past it the tale-of-the-tape textures come back null and sub_8236C5B8 reads null+0x18.
// On a failure we retry the same call with high = the top of the highest window, so the block lands in
// free memory above its own window (heap 27 had 11+ MB free at its own peak). The free path
// (sub_82285130) looks the heap up by address only to pick which lock to hold around XPhysicalFree, so a
// spilled block frees normally.
constexpr uint32_t kWindowTable = 0x8335A8C0, kWindowCount = 0x8335A8A4, kWindowEntry = 64;

inline uint32_t TopOfWindows(uint8_t* base) {
  const uint32_t n = ufc3_heap_trace::Load32(base, kWindowCount);
  uint32_t top = 0;
  for (uint32_t i = 0; i < n && i < 64; ++i) {
    const uint32_t lo = ufc3_heap_trace::Load32(base, kWindowTable + i * kWindowEntry + 4);
    const uint32_t hi = ufc3_heap_trace::Load32(base, kWindowTable + i * kWindowEntry + 8);
    if (hi > lo && hi < 0x20000000) top = std::max(top, hi);
  }
  return top;
}

struct SpillStats {
  std::mutex m;
  uint32_t count = 0;
  uint64_t bytes = 0;
};
inline SpillStats& Spill() {
  static SpillStats s;
  return s;
}

}  // namespace ufc3_phys_trace

extern "C" REX_FUNC(sub_823C75B0) {
  const uint32_t size = ctx.r3.u32, align = ctx.r5.u32, lr = uint32_t(ctx.lr);
  __imp__sub_823C75B0(ctx, base);
  if (REXCVAR_GET(ufc3_trace_physical))
    ufc3_phys_trace::OnAlloc(base, ctx.r3.u32, {size, lr, 0, -1, 0, 0xFFFFFFFF, {}}, "XPhysicalAlloc", align);
}

extern "C" REX_FUNC(sub_823C7638) {
  using namespace ufc3_phys_trace;
  const uint32_t size = ctx.r3.u32, low = ctx.r4.u32, high = ctx.r5.u32, align = ctx.r6.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  Block blk{size, lr, 0, -1, low, high, {}};
  if (lr == kGameAllocLr && REXCVAR_GET(ufc3_trace_physical)) {
    blk.heap = ctx.r25.s32;
    // back-chain: [sp] = caller's sp, the LR into that frame's function is saved at [caller sp - 8]
    uint32_t sp = ctx.r1.u32;
    blk.caller = ufc3_heap_trace::Load32(base, sp + kGameAllocFrame - 8);
    sp = ufc3_heap_trace::Load32(base, sp);  // sub_82284EB8's caller's frame
    for (int i = 0; i < 4; ++i) {
      if (sp < 0x70000000 || sp >= 0x80000000) break;  // guest stacks
      const uint32_t up = ufc3_heap_trace::Load32(base, sp);
      if (up <= sp || up >= 0x80000000) break;
      blk.bt[i] = ufc3_heap_trace::Load32(base, up - 8);
      sp = up;
    }
  }
  const uint32_t protect = ctx.r7.u32;
  __imp__sub_823C7638(ctx, base);
  if (ctx.r3.u32 == 0 && lr == kGameAllocLr && REXCVAR_GET(ufc3_physical_spill)) {
    const uint32_t top = TopOfWindows(base);
    if (top > high) {
      if (REXCVAR_GET(ufc3_trace_physical))
        OnAlloc(base, 0, blk, "XPhysicalAllocEx (own window)", align);
      ctx.r3.u64 = size;
      ctx.r4.u64 = low;
      ctx.r5.u64 = top;
      ctx.r6.u64 = align;
      ctx.r7.u64 = protect;
      ctx.lr = lr;
      __imp__sub_823C7638(ctx, base);
      if (ctx.r3.u32) {
        blk.high = top;
        SpillStats& sp = Spill();
        std::lock_guard lk(sp.m);
        ++sp.count;
        sp.bytes += size;
        if (sp.count <= 20 || (sp.count & (sp.count - 1)) == 0)
          REXLOG_WARN("[phys] heap {} window {:08X}-{:08X} full: {} bytes placed above it at {:08X} "
                      "(spill {}, {:.2f} MB so far)",
                      ctx.r25.s32, low, high, size, ctx.r3.u32, sp.count, Mb(sp.bytes));
      } else {
        REXLOG_WARN("[phys] heap {} window {:08X}-{:08X} full and no room up to {:08X}: {} bytes failed",
                    ctx.r25.s32, low, high, top, size);
      }
    }
  }
  if (REXCVAR_GET(ufc3_trace_physical))
    OnAlloc(base, ctx.r3.u32, blk, "XPhysicalAllocEx", align);
}

extern "C" REX_FUNC(sub_823C76B0) {
  const uint32_t va = ctx.r3.u32, lr = uint32_t(ctx.lr);
  if (REXCVAR_GET(ufc3_trace_physical))
    ufc3_phys_trace::OnFree(va, lr);
  __imp__sub_823C76B0(ctx, base);
}
