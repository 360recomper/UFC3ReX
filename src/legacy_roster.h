// ufc3 - mod fighters for existing saves, and a fighter table past 600.
//
// The fighter table (static_x.mpc sys/plpm, 276-byte records) is copied into
// the system data when a save is created and saved with it (SYSTEM.DAT keeps
// it at sys+0x204C8 with room for 600 records; both loaders, sub_82ED15F0 and
// sub_82ED2088, cap the copy at 600). After that the game only reads the
// save's copy, through [[0x83576FC8]] (u32 count, u32 0, u64 version, then the
// records) and three accessors: by ID (sub_82EDA778, linear search), by index
// (sub_82EDA7D8) and count (sub_82EDA818). Nothing else indexes it.
//
// With ufc3_roster_cap > 0 (default 1024) we point [0x83576FC8] at our own
// guest buffer of that many records: the save's records, followed by every
// record of the overlaid static_x.mpc (<mods_root>/DAT/static_x.mpc) that the
// save lacks. Fighters a mod adds (IDs not in the disc's static_x.mpc) always
// take the mod's current record unless the game has edited them. The save's
// own table is left as the game wrote it, so the save stays readable by a
// build without this hook. Before every save request (sub_82EE9128) the
// records that came from the save are copied back (edits persist), and edited
// extra records (flag 0x40 in word 0, the bit sub_82EDB9E0 merges on) go to
// <user_data_root>/legacy_roster_extra.bin, which is read back when the
// buffer is rebuilt. The buffer is rebuilt whenever the source table's
// address, count, version or end records change (save load, roster update).
//
// ufc3_roster_cap = 0 keeps the old behaviour: append mod records into the
// save's table in place, up to 600.

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(ufc3_merge_mod_fighters, true, "ufc3",
                    "Add fighters from a modded static_x.mpc to the fighter table");

REXCVAR_DEFINE_UINT32(ufc3_roster_cap, 1024, "ufc3",
                      "Fighter table size in records; 0 = the game's own 600-record table");

REXCVAR_DEFINE_BOOL(ufc3_trace_roster, false, "ufc3",
                    "Log fighter-table queries (selectable checks, index walks) with callers");

REX_EXTERN(__imp__sub_8234C280);  // roster builder (walks every record)
REX_EXTERN(__imp__sub_82B99628);  // fight scene setup (loads the slots' move tables)
REX_EXTERN(__imp__sub_82EF1B48);  // move table loader (wazatbl<ID>.bin for a fight slot)
REX_EXTERN(__imp__sub_8234AA58);  // IsSelectable(id, decoded record)
REX_EXTERN(__imp__sub_82EDA7D8);  // fighter ID by table index
REX_EXTERN(__imp__sub_82EDA778);  // record by fighter ID
REX_EXTERN(__imp__sub_82EDA818);  // fighter count
REX_EXTERN(__imp__sub_82EE9128);  // save wrapper: request(kind, params)

namespace ufc3_legacy_roster {

constexpr uint32_t kFighterDbPtr = 0x83576FC8;  // -> {table*}
constexpr uint32_t kRecordSize = 276;
constexpr uint32_t kTableHeader = 16;
constexpr uint32_t kMaxRecords = 600;           // the game's own table
constexpr uint32_t kEditedFlag = 0x40;          // record word 0: set on every record, NOT an edit marker
constexpr bool kSidecarEnabled = false;         // keep edited extra records in the sidecar (no reliable edit marker found)
constexpr char kSidecarName[] = "legacy_roster_extra.bin";
constexpr char kSidecarMagic[4] = {'L', 'R', 'X', '1'};

inline uint32_t BE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

inline uint32_t Load32(uint8_t* base, uint32_t addr) { return BE32(base + addr); }

inline uint64_t Load64(uint8_t* base, uint32_t addr) {
  return (uint64_t(Load32(base, addr)) << 32) | Load32(base, addr + 4);
}

inline void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
  std::memcpy(base + addr, b, 4);
}

inline uint32_t RecId(const uint8_t* rec) { return BE32(rec) >> 16; }

// plpm leaf of an MPC file (node table of 40-byte {tag, name[32], value}).
inline std::vector<uint8_t> ReadPlpm(const std::filesystem::path& mpc) {
  std::ifstream f(mpc, std::ios::binary);
  if (!f) return {};
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.size() < 16) return {};
  uint32_t table_size = BE32(&d[0]), start = BE32(&d[8]), data_start = BE32(&d[12]);
  if (start + table_size > d.size()) return {};
  std::vector<uint8_t> out;
  auto node = [&](uint32_t i, uint32_t& tag, std::string& name, uint32_t& value) {
    const uint8_t* e = &d[start + i * 40];
    tag = BE32(e);
    name.assign(reinterpret_cast<const char*>(e + 4), strnlen(reinterpret_cast<const char*>(e + 4), 32));
    value = BE32(e + 36);
  };
  const char* want[] = {"root", "sys", "plpm"};
  uint32_t first = 0, count = 1;
  for (int depth = 0; depth < 3; ++depth) {
    bool found = false;
    for (uint32_t k = 0; k < count; ++k) {
      uint32_t tag, value;
      std::string name;
      node(first + k, tag, name, value);
      if (name != want[depth]) continue;
      found = true;
      if (depth == 2) {
        if (tag >> 24 != 0xFF) return {};
        uint32_t off = data_start + value, size = (tag & 0xFFFFFF) * 32;
        if (off + size > d.size()) return {};
        out.assign(d.begin() + off, d.begin() + off + size);
      } else {
        first = value;
        count = tag & 0xFFFF;
      }
      break;
    }
    if (!found) return {};
  }
  return out;
}

// Records of a plpm blob (count at 0, records from kTableHeader).
inline std::vector<const uint8_t*> Records(const std::vector<uint8_t>& plpm) {
  std::vector<const uint8_t*> out;
  if (plpm.size() < kTableHeader) return out;
  uint32_t n = BE32(&plpm[0]);
  if (kTableHeader + uint64_t(n) * kRecordSize > plpm.size()) return out;
  for (uint32_t i = 0; i < n; ++i) out.push_back(&plpm[kTableHeader + i * kRecordSize]);
  return out;
}

// Mod plpm, read once. Empty when there is no modded static_x.mpc.
inline const std::vector<uint8_t>& ModPlpm() {
  static std::vector<uint8_t> plpm;
  static bool loaded = false;
  if (loaded) return plpm;
  loaded = true;
  std::string enabled = rex::cvar::GetFlagByName("mods_enabled");
  std::string mods = rex::cvar::GetFlagByName("mods_root");
  if (enabled == "false" || mods.empty()) {
    REXLOG_INFO("[roster] no mods (mods_enabled={}, mods_root='{}'); nothing to merge", enabled, mods);
    return plpm;
  }
  auto path = std::filesystem::path(mods) / "DAT" / "static_x.mpc";
  plpm = ReadPlpm(path);
  if (Records(plpm).empty()) {
    REXLOG_INFO("[roster] no usable plpm in {}; nothing to merge", path.string());
    plpm.clear();
  } else {
    REXLOG_INFO("[roster] mod plpm: {} records from {}", BE32(&plpm[0]), path.string());
  }
  return plpm;
}

// IDs on the disc (the unmodded static_x.mpc), to tell mod-added fighters apart.
inline const std::unordered_set<uint32_t>& DiscIds() {
  static std::unordered_set<uint32_t> ids;
  static bool loaded = false;
  if (loaded) return ids;
  loaded = true;
  std::string root = rex::cvar::GetFlagByName("game_data_root");
  auto plpm = ReadPlpm(std::filesystem::path(root) / "DAT" / "static_x.mpc");
  for (auto* r : Records(plpm)) ids.insert(RecId(r));
  REXLOG_INFO("[roster] disc fighter table: {} records", ids.size());
  return ids;
}

inline std::filesystem::path SidecarPath() {
  return std::filesystem::path(rex::cvar::GetFlagByName("user_data_root")) / kSidecarName;
}

// Edited extra records saved by an earlier session, by ID.
inline std::unordered_map<uint32_t, std::vector<uint8_t>> ReadSidecar() {
  std::unordered_map<uint32_t, std::vector<uint8_t>> out;
  std::ifstream f(SidecarPath(), std::ios::binary);
  if (!f) return out;
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.size() < 8 || std::memcmp(d.data(), kSidecarMagic, 4) != 0) return out;
  uint32_t n = BE32(&d[4]);
  if (8 + uint64_t(n) * kRecordSize > d.size()) return out;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* r = &d[8 + i * kRecordSize];
    out[RecId(r)] = std::vector<uint8_t>(r, r + kRecordSize);
  }
  return out;
}

// Legacy mode (cap 0): append mod records into the game's table, up to 600.
inline void MergeInPlace(uint8_t* base, const char* from) {
  static uint32_t merged_table = 0, merged_count = 0, merged_last = 0;
  uint32_t db = Load32(base, kFighterDbPtr);
  uint32_t table = db ? Load32(base, db) : 0;
  if (!table) return;
  uint32_t count = Load32(base, table);
  if (count == 0 || count > kMaxRecords) return;
  uint32_t last = Load32(base, table + kTableHeader + (count - 1) * kRecordSize);
  if (table == merged_table && count == merged_count && last == merged_last) return;
  std::unordered_set<uint32_t> have;
  for (uint32_t i = 0; i < count; ++i) have.insert(Load32(base, table + kTableHeader + i * kRecordSize) >> 16);
  uint32_t before = count;
  for (auto* rec : Records(ModPlpm())) {
    if (have.count(RecId(rec))) continue;
    if (count >= kMaxRecords) {
      REXLOG_WARN("[roster] fighter table full ({}), skipping mod fighter {}", kMaxRecords, RecId(rec));
      continue;
    }
    std::memcpy(base + table + kTableHeader + count * kRecordSize, rec, kRecordSize);
    ++count;
  }
  if (count != before) Store32(base, table, count);
  REXLOG_INFO("[roster] {}: table {:08X}, {} -> {} records (in place)", from, table, before, count);
  merged_table = table;
  merged_count = count;
  merged_last = Load32(base, table + kTableHeader + (count - 1) * kRecordSize);
}

struct Relocated {
  std::mutex lock;
  uint32_t buffer = 0, cap = 0;     // our table (guest address) and its size in records
  uint32_t src = 0, src_count = 0;  // the game's table we built it from
  uint64_t src_version = 0;
  uint32_t src_first = 0, src_last = 0;
  // Save records of mod fighters the current mod no longer has, hidden under a
  // spare ID in our table; SyncForSave puts the originals back.
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> orphans;
};

// Spare IDs for orphans: the disc plpm has no fighter in 930-989 (it does use
// 990-993 and 999, so these must stay below 990), sub_82C35938 does not
// special-case them (it handles 700-929, 990-991, ...), the roster builder only
// lists 1-498, and the unlock bitfield still covers them.
constexpr uint32_t kOrphanIdTop = 989, kOrphanIdBottom = 930;

inline Relocated& State() {
  static Relocated s;
  return s;
}

inline bool SourceUnchanged(uint8_t* base, const Relocated& st) {
  uint32_t n = Load32(base, st.src);
  return n == st.src_count && Load64(base, st.src + 8) == st.src_version && n > 0 &&
         Load32(base, st.src + kTableHeader) == st.src_first &&
         Load32(base, st.src + kTableHeader + (n - 1) * kRecordSize) == st.src_last;
}

inline void RememberSource(uint8_t* base, Relocated& st, uint32_t src) {
  uint32_t n = Load32(base, src);
  st.src = src;
  st.src_count = n;
  st.src_version = Load64(base, src + 8);
  st.src_first = Load32(base, src + kTableHeader);
  st.src_last = Load32(base, src + kTableHeader + (n - 1) * kRecordSize);
}

// Called on every fighter-table access; the common path is a few loads.
inline void MergeModFighters(uint8_t* base, const char* from) {
  if (!REXCVAR_GET(ufc3_merge_mod_fighters)) return;
  if (REXCVAR_GET(ufc3_roster_cap) == 0) {
    MergeInPlace(base, from);
    return;
  }
  uint32_t db = Load32(base, kFighterDbPtr);
  if (!db) return;
  auto& st = State();
  uint32_t cur = Load32(base, db);
  if (st.buffer && cur == st.buffer && st.src && SourceUnchanged(base, st)) return;

  std::lock_guard<std::mutex> guard(st.lock);
  cur = Load32(base, db);
  uint32_t src = (st.buffer && cur == st.buffer) ? st.src : cur;
  if (!src) return;
  uint32_t n = Load32(base, src);
  if (n == 0 || n > kMaxRecords) return;  // not loaded yet, or not a table
  if (!st.buffer) {
    uint32_t cap = std::max<uint32_t>(REXCVAR_GET(ufc3_roster_cap), kMaxRecords);
    st.buffer = REX_KERNEL_MEMORY()->SystemHeapAlloc(kTableHeader + cap * kRecordSize);
    if (!st.buffer) {
      REXLOG_ERROR("[roster] could not allocate a {}-record fighter table; using the game's 600", cap);
      REXCVAR_SET(ufc3_roster_cap, 0u);
      return;
    }
    st.cap = cap;
    REXLOG_INFO("[roster] fighter table cap {} (the game's own: {}), buffer {:08X}", cap, kMaxRecords,
                st.buffer);
  }
  const auto& disc = DiscIds();
  std::unordered_map<uint32_t, const uint8_t*> mod;
  for (auto* r : Records(ModPlpm())) mod[RecId(r)] = r;
  auto sidecar = kSidecarEnabled ? ReadSidecar() : decltype(ReadSidecar()){};  // older builds' files held stale copies

  // Header and the save's records first, in their order (SyncForSave relies on it).
  std::memcpy(base + st.buffer, base + src, kTableHeader);
  std::unordered_set<uint32_t> have;
  uint32_t count = 0, refreshed = 0;
  st.orphans.clear();
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t at = st.buffer + kTableHeader + count * kRecordSize;
    std::memcpy(base + at, base + src + kTableHeader + i * kRecordSize, kRecordSize);
    uint32_t id = Load32(base, at) >> 16;
    auto m = mod.find(id);
    // Flag 0x40 is set on every record the game has (disc ones included), so
    // it cannot mean "edited": gating on it left every stale copy of a mod
    // fighter in the save (e.g. Werdum with an older build's name flag) unrefreshed.
    if (!disc.count(id) && m != mod.end()) {
      std::memcpy(base + at, m->second, kRecordSize);  // mod fighter an older build merged in
      ++refreshed;
    } else if (!disc.count(id) && kOrphanIdTop - st.orphans.size() >= kOrphanIdBottom) {
      // A mod fighter an older build merged in that this mod dropped: his model
      // is gone, so selecting him would crash. Hide him; the save keeps him.
      st.orphans.emplace_back(i, std::vector<uint8_t>(base + at, base + at + kRecordSize));
      uint32_t hidden = kOrphanIdTop - uint32_t(st.orphans.size() - 1);
      Store32(base, at, (hidden << 16) | (Load32(base, at) & 0xFFFF));
      REXLOG_INFO("[roster] save fighter {} is not in the mod: hidden as {}", id, hidden);
    }
    have.insert(id);
    ++count;
  }
  uint32_t added = 0, from_sidecar = 0, dropped = 0;
  for (auto* r : Records(ModPlpm())) {
    uint32_t id = RecId(r);
    if (have.count(id)) continue;
    if (count >= st.cap) {
      ++dropped;
      continue;
    }
    const uint8_t* use = r;
    auto it = sidecar.find(id);
    if (it != sidecar.end()) {
      use = it->second.data();
      ++from_sidecar;
    }
    std::memcpy(base + st.buffer + kTableHeader + count * kRecordSize, use, kRecordSize);
    have.insert(id);
    ++count;
    ++added;
  }
  Store32(base, st.buffer, count);
  Store32(base, db, st.buffer);
  RememberSource(base, st, src);
  REXLOG_INFO("[roster] {}: fighter table {:08X} -> {:08X}: {} from the save ({} mod records "
              "refreshed) + {} from the mod ({} edited ones from {}) = {} of {}",
              from, src, st.buffer, n, refreshed, added, from_sidecar, kSidecarName, count, st.cap);
  if (dropped) {
    REXLOG_WARN("[roster] {} mod fighters did not fit (ufc3_roster_cap {})", dropped, st.cap);
  }
}

// Before a save: copy the save's records back into its own table, and keep
// edited extra records in the sidecar.
inline void SyncForSave(uint8_t* base) {
  auto& st = State();
  if (!st.buffer || !st.src) return;
  std::lock_guard<std::mutex> guard(st.lock);
  uint32_t db = Load32(base, kFighterDbPtr);
  if (!db || Load32(base, db) != st.buffer) return;
  uint32_t n = st.src_count;
  std::memcpy(base + st.src + 8, base + st.buffer + 8, 8);  // version
  std::memcpy(base + st.src + kTableHeader, base + st.buffer + kTableHeader, size_t(n) * kRecordSize);
  for (auto& [i, rec] : st.orphans) {
    if (i < n) std::memcpy(base + st.src + kTableHeader + i * kRecordSize, rec.data(), kRecordSize);
  }
  RememberSource(base, st, st.src);
  uint32_t count = Load32(base, st.buffer);
  std::vector<uint8_t> out(kSidecarMagic, kSidecarMagic + 4);
  out.resize(8);
  uint32_t edited = 0;
  for (uint32_t i = n; i < count; ++i) {
    uint32_t at = st.buffer + kTableHeader + i * kRecordSize;
    // Nothing is "edited" yet: the flag below is on every record, and mod
    // fighters always come back from the mod (see MergeModFighters).
    if (!kSidecarEnabled) continue;
    out.insert(out.end(), base + at, base + at + kRecordSize);
    ++edited;
  }
  out[4] = uint8_t(edited >> 24);
  out[5] = uint8_t(edited >> 16);
  out[6] = uint8_t(edited >> 8);
  out[7] = uint8_t(edited);
  std::error_code ec;
  auto path = SidecarPath();
  if (edited == 0) {
    std::filesystem::remove(path, ec);
  } else {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(out.data()), out.size());
  }
  REXLOG_INFO("[roster] save: {} records synced to the save's table, {} edited extra records to {}", n,
              edited, path.string());
}

inline bool Trace(uint32_t& n) { return REXCVAR_GET(ufc3_trace_roster) && ++n <= 3000; }

}  // namespace ufc3_legacy_roster

extern "C" REX_FUNC(sub_8234C280) {
  ufc3_legacy_roster::MergeModFighters(base, "roster builder");
  __imp__sub_8234C280(ctx, base);
}

extern "C" REX_FUNC(sub_82EDA818) {
  ufc3_legacy_roster::MergeModFighters(base, "count");
  __imp__sub_82EDA818(ctx, base);
}

// Also the --ufc3_trace_imports save-request log (import_trace.h).
extern "C" REX_FUNC(sub_82EE9128) {
  ufc3_legacy_roster::SyncForSave(base);
  uint32_t r4 = ctx.r4.u32, r5 = ctx.r5.u32, lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82EE9128(ctx, base);
  if (REXCVAR_GET(ufc3_trace_imports)) {
    REXLOG_INFO("[save] request 82EE9128(r4={:08X}, r5={:08X}) lr={:08X} -> {:08X}", r4, r5, lr,
                ctx.r3.u32);
  }
}

// Every by-ID record read goes through here, so merging here covers rosters,
// match setup and fight loading.
extern "C" REX_FUNC(sub_82EDA778) {
  ufc3_legacy_roster::MergeModFighters(base, "record lookup");
  static uint32_t n = 0;
  uint32_t id = ctx.r4.u32, lr = uint32_t(ctx.lr);
  __imp__sub_82EDA778(ctx, base);
  if (ufc3_legacy_roster::Trace(n)) {
    REXLOG_INFO("[roster] lookup id {} -> {:08X} (lr {:08X})", id, ctx.r3.u32, lr);
  }
}

extern "C" REX_FUNC(sub_8234AA58) {
  static uint32_t n = 0;
  uint32_t id = ctx.r3.u32, lr = uint32_t(ctx.lr);
  __imp__sub_8234AA58(ctx, base);
  if (ufc3_legacy_roster::Trace(n)) {
    REXLOG_INFO("[roster] selectable({}) = {} (lr {:08X})", id, ctx.r3.u32, lr);
  }
}

extern "C" REX_FUNC(sub_82EDA7D8) {
  static uint32_t n = 0;
  uint32_t idx = ctx.r4.u32, lr = uint32_t(ctx.lr);
  __imp__sub_82EDA7D8(ctx, base);
  if (ufc3_legacy_roster::Trace(n)) {
    REXLOG_INFO("[roster] index {} -> id {} (lr {:08X})", idx, ctx.r3.u32, lr);
  }
}

// Fight setup asks for each slot's move table here: wazatbl<r6>.bin, or the
// default when r6 < 0 or the file isn't in the table pack.
extern "C" REX_FUNC(sub_82EF1B48) {
  static uint32_t n = 0;
  uint32_t slot = ctx.r5.u32, id = ctx.r6.u32, lr = uint32_t(ctx.lr);
  __imp__sub_82EF1B48(ctx, base);
  if (ufc3_legacy_roster::Trace(n)) {
    REXLOG_INFO("[roster] move table slot {} id {} (lr {:08X})", slot, int32_t(id), lr);
  }
}

// Debug: the fight scene object's words around the move-table ids (+3560..+3760).
extern "C" REX_FUNC(sub_82B99628) {
  static uint32_t n = 0;
  uint32_t self = ctx.r3.u32;
  if (ufc3_legacy_roster::Trace(n)) {
    std::string line;
    for (uint32_t o = 3560; o < 3760; o += 4) {
      uint32_t v = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + self + o));
      char b[32];
      std::snprintf(b, sizeof b, " %u=%X", o, v);
      line += b;
    }
    REXLOG_INFO("[roster] fight scene {:08X}:{}", self, line);
  }
  __imp__sub_82B99628(ctx, base);
}
