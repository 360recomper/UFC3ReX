// ufc3 - match-load position graph without the null read (--ufc3_position_guard,
// on by default). FINDINGS-quarry-crash.md.
//
// At match load sub_82905C48 builds, per player p = 0/1, the list of positions
// that fighter can be in: obj + 12 + (p*128 + i)*852, 852-byte entries
// (+0 position ID, +4 4 bytes from sys/posTbl, +12 / +428 lists of target
// position IDs, counts at +844 / +848); the count is at obj + 4 + 4p.
// sub_829030A8(obj, p) then walks both lists of every entry and appends each
// target not in the list yet, copying its 4 bytes from posTbl ([0x8357498C]:
// +4 count, +8 8-byte entries). A target outside posTbl makes the lookup
// return null and the copy reads guest address 0 (Liddell vs Quarry, PRIDE,
// later load in a session). It also never caps the list at its 128 entries.
//
// Each list is 32 slots: IDs at +0, 5-byte values at +128, 4-byte values at
// +288 (sub_82904548 sorts the three together). The next pass, sub_82903898,
// looks up every list target's entry to add the reverse link and reads its
// +844/+848 without a null check, so a target left in a list without an entry
// reads guest 0x34C/0x350 (Houston, 1x, FINDINGS-quarry-crash.md Round 2).
//
// With the option on this is a C++ copy of sub_829030A8 (same order, same
// duplicate test, returns the new count in r3). A target outside posTbl, or
// one that doesn't fit in the 128 entries, is removed from its list (all three
// arrays, count lowered) instead of appended, each logged once (WARN). Off =
// the original. --ufc3_trace_positions also logs every move end-position
// lookup (sub_82904738) whose result is outside the table, with the 8-char
// tag it looked up. (sub_82C87670, the lookup itself, is skip_trace's.)

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <tuple>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(ufc3_position_guard, true, "ufc3",
                    "Match-load position graph: skip transition targets outside sys/posTbl "
                    "instead of reading guest address 0 (Liddell vs Quarry PRIDE crash)");
REXCVAR_DEFINE_BOOL(ufc3_trace_positions, false, "ufc3",
                    "Log move end-position lookups (sub_82904738) that give an ID outside sys/posTbl");

REX_EXTERN(__imp__sub_829030A8);  // add the transition targets of player p's positions
REX_EXTERN(__imp__sub_82904738);  // resolve a move's end position (r7 = out ID)

namespace ufc3_position_guard {

constexpr uint32_t kPosTbl = 0x8357498C;       // -> {+4 count, +8 entries (8 bytes)}
constexpr uint32_t kCornerBase = 0x83541FE0;   // corner record, +0 = fighter id * 100 (fighter_music.h)
constexpr uint32_t kCornerStride = 20712;
constexpr uint32_t kEntrySize = 852;
constexpr uint32_t kMaxEntries = 128;
constexpr uint32_t kListSlots = 32;            // IDs +0, 5-byte values +128, 4-byte values +288
constexpr uint32_t kListValues5 = 128;
constexpr uint32_t kListValues4 = 288;
constexpr uint32_t kTagFrame = 3776;          // sub_82904738 frame; its "<5 chars>_10" tag is at sp+104

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}

inline void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = _byteswap_ulong(v);
  std::memcpy(base + addr, &v, 4);
}

struct State {
  std::mutex mutex;
  std::set<std::tuple<uint32_t, uint32_t, int32_t, uint32_t>> logged;  // p, source pos, target, kind
};

inline State& Get() {
  static State s;
  return s;
}

inline uint32_t CornerFighter(uint8_t* base, uint32_t p) {
  return Load32(base, kCornerBase + p * kCornerStride) / 100;
}

// Logs one skipped target once per (player, source, target, kind).
inline void LogSkip(uint8_t* base, uint32_t p, uint32_t src, int32_t target, uint32_t list, uint32_t slot,
                    uint32_t kind, uint32_t table_count) {
  State& st = Get();
  std::lock_guard lock(st.mutex);
  if (st.logged.size() >= 256 || !st.logged.emplace(p, src, target, kind).second) return;
  if (kind == 0)
    REXLOG_WARN("[positions] player {} (corner fighter {}): position {} list {} slot {}: target {} is outside "
                "posTbl (count {}), removed from the list",
                p, CornerFighter(base, p), src, list, slot, target, table_count);
  else if (kind == 1)
    REXLOG_WARN("[positions] player {} (corner fighter {}): position {} list {} slot {}: target {} removed, "
                "the position list is full ({} entries)",
                p, CornerFighter(base, p), src, list, slot, target, kMaxEntries);
  else
    REXLOG_WARN("[positions] player {} (corner fighter {}): position {} list {} count {} clamped to {}", p,
                CornerFighter(base, p), src, list, target, kListSlots);
}

// Removes slot j of a cnt-slot list (all three arrays) and clears the freed
// last slot the way sub_82902C08 leaves an empty one.
inline void RemoveSlot(uint8_t* base, uint32_t items, int32_t cnt, int32_t j) {
  for (int32_t k = j; k + 1 < cnt; ++k) {
    std::memcpy(base + items + 4 * k, base + items + 4 * (k + 1), 4);
    std::memcpy(base + items + kListValues5 + 5 * k, base + items + kListValues5 + 5 * (k + 1), 5);
    std::memcpy(base + items + kListValues4 + 4 * k, base + items + kListValues4 + 4 * (k + 1), 4);
  }
  const uint32_t last = static_cast<uint32_t>(cnt - 1);
  std::memset(base + items + 4 * last, 0, 4);
  std::memset(base + items + kListValues5 + 5 * last, 0, 5);
  std::memset(base + items + kListValues4 + 4 * last, 0xFF, 4);
}

}  // namespace ufc3_position_guard

extern "C" REX_FUNC(sub_829030A8) {
  namespace pg = ufc3_position_guard;
  const uint32_t obj = ctx.r3.u32, p = ctx.r4.u32;
  if (!REXCVAR_GET(ufc3_position_guard) || p > 1) {
    __imp__sub_829030A8(ctx, base);
    return;
  }
  auto entry = [&](uint32_t k) { return obj + 12 + (p * pg::kMaxEntries + k) * pg::kEntrySize; };
  const uint32_t tbl = pg::Load32(base, pg::kPosTbl);
  const uint32_t tbl_entries = tbl ? pg::Load32(base, tbl + 8) : 0;
  const int32_t tbl_count = tbl ? static_cast<int32_t>(pg::Load32(base, tbl + 4)) : 0;

  int32_t n = static_cast<int32_t>(pg::Load32(base, obj + 4 + 4 * p));
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t e = entry(i);
    for (uint32_t list = 1; list <= 2; ++list) {
      const uint32_t items = e + (list == 1 ? 12 : 428);
      const uint32_t count_addr = e + (list == 1 ? 844 : 848);
      int32_t cnt = static_cast<int32_t>(pg::Load32(base, count_addr));
      if (cnt > static_cast<int32_t>(pg::kListSlots)) {
        pg::LogSkip(base, p, pg::Load32(base, e), cnt, list, 0, 2, 0);
        cnt = pg::kListSlots;
        pg::Store32(base, count_addr, static_cast<uint32_t>(cnt));
      }
      for (int32_t j = 0; j < cnt; ++j) {
        const int32_t target = static_cast<int32_t>(pg::Load32(base, items + 4 * j));
        bool known = false;
        for (int32_t k = 0; k < n && !known; ++k)  // the original skips the entry itself (k == i)
          known = k != i && static_cast<int32_t>(pg::Load32(base, entry(k))) == target;
        if (known) continue;
        const bool outside = !tbl_entries || target < 0 || target >= tbl_count;
        if (outside || n >= static_cast<int32_t>(pg::kMaxEntries)) {
          pg::LogSkip(base, p, pg::Load32(base, e), target, list, j, outside ? 0 : 1, tbl_count);
          pg::RemoveSlot(base, items, cnt, j);
          pg::Store32(base, count_addr, static_cast<uint32_t>(--cnt));
          --j;
          continue;
        }
        const uint32_t dst = entry(n);
        pg::Store32(base, dst, static_cast<uint32_t>(target));
        std::memcpy(base + dst + 4, base + tbl_entries + static_cast<uint32_t>(target) * 8, 4);
        ++n;
      }
    }
  }
  ctx.r3.s64 = n;
}

// sub_82904738(.., r7 = out ID) resolves one move's end position; with the trace
// on, an out ID outside posTbl is logged with the tag left in its frame (only
// meaningful when it took the "<5 chars>_10" path, the frame's sp+104).
extern "C" REX_FUNC(sub_82904738) {
  namespace pg = ufc3_position_guard;
  if (!REXCVAR_GET(ufc3_trace_positions)) {
    __imp__sub_82904738(ctx, base);
    return;
  }
  const uint32_t out = ctx.r7.u32, frame = ctx.r1.u32 - pg::kTagFrame;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82904738(ctx, base);
  const uint32_t tbl = pg::Load32(base, pg::kPosTbl);
  if (!out || !tbl) return;
  const int32_t id = static_cast<int32_t>(pg::Load32(base, out));
  const int32_t count = static_cast<int32_t>(pg::Load32(base, tbl + 4));
  if (id >= 0 && id < count) return;
  char tag[9] = {};
  std::memcpy(tag, base + frame + 104, 8);
  for (char& c : tag)
    if (c && (c < 0x20 || c > 0x7E)) c = '?';
  REXLOG_WARN("[positions] end position lookup: out {:08X} = {} (posTbl count {}), tag \"{}\", lr {:08X}", out, id,
              count, tag, lr);
}
