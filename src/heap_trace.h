// ufc3 - log the game's heap failures (--ufc3_trace_heap).
//
// The game has its own heap manager: a table of 44-byte heap entries at
// [0x833CEDF8] (+0 start, +4 end, +28 name, +36 used and +40 peak, both in
// 16-byte units), a per-thread current heap (sub_8279C6C8), and the
// allocator sub_8279CF88(size, flags), which returns 0 when no free block in
// the current heap fits. operator new (sub_82220188) ends up there.
//
// With the trace on, every failed allocation is logged with the heap's
// name and fill level, and every fighter-model load (sub_82BCF1D8, the
// chara/<ID>/<ID>.pac loader) with the heap it loads into. This is how to
// see which budget a too-big mod model exceeds.

#pragma once

#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(ufc3_trace_heap, false, "ufc3",
                    "Log failed game-heap allocations and fighter model loads");

REX_EXTERN(__imp__sub_8279CF88);  // heap alloc(size, flags) in the current heap
REX_EXTERN(__imp__sub_82BCF1D8);  // fighter model loader (vtable 0x82135BE8 slot 2)
REX_EXTERN(sub_8279C6C8);         // current heap id of this thread

namespace ufc3_heap_trace {

constexpr uint32_t kHeapTablePtr = 0x833CEDF8;
constexpr uint32_t kHeapEntry = 44;

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}

inline std::string HeapInfo(uint8_t* base, uint32_t id) {
  uint32_t table = Load32(base, kHeapTablePtr);
  if (!table || id > 64) return "?";
  uint32_t e = table + id * kHeapEntry;
  uint32_t start = Load32(base, e), end = Load32(base, e + 4);
  char name[9] = {};
  std::memcpy(name, base + e + 28, 8);
  for (char& c : name) {
    if (c && (c < 32 || c > 126)) c = '?';
  }
  return fmt::format("heap {} '{}' {:.2f} MB, used {:.2f} MB, peak {:.2f} MB", id, name,
                     (end - start) / 1048576.0, Load32(base, e + 36) * 16 / 1048576.0,
                     Load32(base, e + 40) * 16 / 1048576.0);
}

inline uint32_t CurrentHeap(PPCContext& ctx, uint8_t* base) {
  PPCContext c = ctx;
  sub_8279C6C8(c, base);
  return c.r3.u32;
}

}  // namespace ufc3_heap_trace

extern "C" REX_FUNC(sub_8279CF88) {
  using namespace ufc3_heap_trace;
  uint32_t size = ctx.r3.u32, lr = uint32_t(ctx.lr);
  bool trace = REXCVAR_GET(ufc3_trace_heap);
  uint32_t heap = trace ? CurrentHeap(ctx, base) : 0;
  __imp__sub_8279CF88(ctx, base);
  if (trace && ctx.r3.u32 == 0) {
    static uint32_t n = 0;
    if (++n <= 200) {
      REXLOG_WARN("[heap] alloc of {:.2f} MB ({} bytes) failed in {} (lr {:08X})",
                  size / 1048576.0, size, HeapInfo(base, heap), lr);
    }
  }
}

extern "C" REX_FUNC(sub_82BCF1D8) {
  using namespace ufc3_heap_trace;
  if (REXCVAR_GET(ufc3_trace_heap)) {
    uint32_t self = ctx.r3.u32;
    REXLOG_INFO("[heap] fighter model load: id {} (raw {}), loader {:08X}, {}", ctx.r4.s32 / 100,
                ctx.r4.s32, self, HeapInfo(base, Load32(base, self + 40)));
  }
  __imp__sub_82BCF1D8(ctx, base);
}
