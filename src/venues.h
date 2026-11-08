// ufc3 - extra exhibition venues: an existing arena with a mod's cage mat or
// ring canvas (and mat logos) swapped in.
//
// How the game picks a venue (found 2026-10-20):
// - The match settings ([0x83541FE0], the block the whole game reads) hold the
//   stage number at +0x32914, a variant at +0x32918 and the cage/ring model
//   index at +0x3291C. SetStage (sub_82ED9C08(settings, stage, variant, oct))
//   decides everything by the stage number's range: 0-19 UFC cage
//   (octagon_00), 20-29 PRIDE ring (octagon_02), 30-39 small cage
//   (octagon_01), 40-49 gym. oct = 16 means "from the range".
// - The arena is stage/<stage*100+variant>.pac; the cage or ring is
//   stage/octagon_<oct>.pac and its mat logos stage/<stage*100>_00_octlogo.tex
//   (sub_82C42DB0 formats both from the strings at 0x82139FE4 / 0x82139FC8,
//   read through the pointer table at 0x83335808: [0] and [1] the cage/ring
//   name, [2] the logo name; the strings themselves are read-only).
//   The variant is 0 everywhere on the disc and other code builds lookup keys
//   from stage*100+variant ("%04d", "%04d_logos"), so it is left alone here.
// - The exhibition venue screen (vtable 0x82107DB0..) is a 9-slot carousel:
//   item count at +224 (9 UFC, 1 PRIDE, 6 other), 64-byte UTF-16BE names at
//   +228, the cursor at +804, then 8-byte thumbnail and picture records at
//   +808 / +880. The cursor maps to a stage through the table at 0x82107C54
//   (sub_8299C410; PRIDE is always stage 20). The records fill the object up
//   to the next field, so the list cannot grow in place.
//
// What this does, driven by <mods_root>/DAT/stage/venues.txt:
// - The venue screen gets the extra entries as a rolling window: while the
//   screen draws (sub_8299B5F0) or reads input (sub_8299B9C0), the 9 slots
//   around the cursor are filled from a longer virtual list, then the stock
//   slots are put back, so the screen's own teardown sees only its own data.
//   A new entry shows its base arena's pictures and its own name.
// - Confirming an extra entry (sub_829C3180 and the two other venue confirm
//   paths) sets the base arena's stage as usual and marks the venue active.
//   Any later SetStage with another stage clears it.
// - While a venue is active and the stage is its base arena, the cage/ring
//   loader reads stage/octagon_<oct>_v<id>.pac and stage/<stage>_<id>_octlogo.tex
//   instead of the stock files (each only if venues.txt says the venue has one).
// - The PRIDE screen ignores all input in its handler (sub_8299B9C0 resets
//   the cursors and returns while the mode object at [0x834458F8] answers
//   "PRIDE" through its vfunc +36: venue and referee are fixed there). With
//   extra PRIDE venues, that one call answers "not PRIDE" for the handler
//   only (the object's vtable pointer points at a copy whose +36 entry is
//   sub_82F654C8, "return 0", during the call), so the stock list code runs:
//   cursor move, scroll animations, name and strip update, as on the UFC
//   list, with the stock keys for stepping and for the arena/referee
//   columns (UP/DOWN and stick up/down step, LEFT/RIGHT switch columns). In
//   the referee column the PRIDE referee stays: a referee step is taken back.
//   The entry's name is written into the slot the screen shows and left
//   there: the screen's text node keeps the buffer's address and reads it
//   when it draws, after the handler.
//
// venues.txt, one venue per line ('#' starts a comment):
//   <id 1-99> <ufc|pride> <base stage> <has octagon 0/1> <logo id, 0 = stock> <NAME ...>

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "bare_knuckle.h"   // ufc3_bk::g_on_pad

REXCVAR_DEFINE_BOOL(ufc3_extra_venues, true, "ufc3",
                    "Add the venues from <mods_root>/DAT/stage/venues.txt to the exhibition venue list");
REXCVAR_DEFINE_BOOL(ufc3_trace_venues, false, "ufc3",
                    "Log venue screen moves, venue confirms, SetStage calls and cage/ring loads");

REX_EXTERN(__imp__sub_8299C678);  // venue screen: enter (fills the list)
REX_EXTERN(__imp__sub_8299B9C0);  // venue screen: input
REX_EXTERN(__imp__sub_8299B5F0);  // venue screen: thumbnail strip + picture
REX_EXTERN(__imp__sub_8299C188);  // venue screen: restore cursor from a stage
REX_EXTERN(__imp__sub_8299C410);  // venue screen: stage of the cursor
REX_EXTERN(__imp__sub_8299B8C8);  // venue screen: cursor on "Any"?
REX_EXTERN(__imp__sub_829C3180);  // exhibition: confirm venue/referee
REX_EXTERN(__imp__sub_82A76CD8);  // other venue confirm path
REX_EXTERN(__imp__sub_82A1DA50);  // other venue confirm path
REX_EXTERN(__imp__sub_82ED9C08);  // SetStage(settings, stage, variant, oct)
REX_EXTERN(__imp__sub_82C42DB0);  // cage/ring loader (octagon object, oct index)
REX_EXTERN(sub_8299B5F0);         // the hook below (redraws the strip with our records)

namespace ufc3_venues {

constexpr uint32_t kSettings = 0x83541FE0;
constexpr uint32_t kStage = kSettings + 0x32914;
constexpr uint32_t kUfcTable = 0x82107C54;   // stage of cursor 1..8 (UFC list)
constexpr uint32_t kFmtTable = 0x83335808;   // -> cage/ring name x2, mat-logo name (.data)
constexpr uint32_t kFmtRoom = 32;
constexpr uint32_t kModePtr = 0x834458F8;   // mode object; vfunc +36 = "venue/referee fixed" (PRIDE)
constexpr uint32_t kReturnZero = 0x82F654C8;  // sub_82F654C8: li r3,0; blr
constexpr uint32_t kVtableCopy = 64;          // vtable entries copied

constexpr uint32_t kCount = 224, kNames = 228, kSel = 804, kThumbs = 808, kPics = 880;
constexpr int kSlots = 9, kNameBytes = 64, kRecBytes = 8;

struct Venue {
  int id = 0;
  bool pride = false;
  int base = 0;
  bool octagon = false;
  int logo = 0;
  std::string name;
  bool art[2] = {false, false};   // venue_<id>_thumb.dds / venue_<id>_picture.dds in DAT/stage
};

inline uint32_t Load32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return _byteswap_ulong(v);
}
inline void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  v = _byteswap_ulong(v);
  std::memcpy(base + a, &v, 4);
}

// A copy of the mode object's vtable whose +36 entry returns 0, made once per real vtable.
inline uint32_t FakeModeVtable(uint8_t* base, uint32_t real_vt) {
  static uint32_t made_for = 0, fake = 0;
  if (real_vt == made_for && fake) return fake;
  if (!fake) fake = REX_KERNEL_MEMORY()->SystemHeapAlloc(kVtableCopy * 4);
  if (!fake) return 0;
  std::memcpy(base + fake, base + real_vt, kVtableCopy * 4);
  Store32(base, fake + 36, kReturnZero);
  made_for = real_vt;
  return fake;
}

// Cage/ring model index of a stage, as SetStage picks it.
inline int OctOf(int stage) {
  if (stage >= 20 && stage <= 29) return 2;
  if (stage >= 30 && stage <= 39) return 1;
  return 0;
}

inline std::string OctFile(const Venue& v) {
  char b[64];
  std::snprintf(b, sizeof b, "octagon_%02d_v%02d.pac", OctOf(v.base), v.id);
  return b;
}
inline std::string ArtFile(const Venue& v, int k) {
  char b[64];
  std::snprintf(b, sizeof b, "venue_%02d_%s.dds", v.id, k ? "picture" : "thumb");
  return b;
}
inline std::filesystem::path StageDir() {
  return std::filesystem::path(rex::cvar::GetFlagByName("mods_root")) / "DAT" / "stage";
}
inline std::string LogoFile(const Venue& v) {
  char b[64];
  std::snprintf(b, sizeof b, "%04d_%02d_octlogo.tex", v.base * 100, v.logo);
  return b;
}

inline const std::vector<Venue>& Venues() {
  static std::vector<Venue> list;
  static bool loaded = false;
  if (loaded) return list;
  loaded = true;
  std::string enabled = rex::cvar::GetFlagByName("mods_enabled");
  std::string mods = rex::cvar::GetFlagByName("mods_root");
  if (!REXCVAR_GET(ufc3_extra_venues) || enabled == "false" || mods.empty()) return list;
  auto dir = std::filesystem::path(mods) / "DAT" / "stage";
  std::ifstream f(dir / "venues.txt");
  if (!f) return list;
  std::string line;
  while (std::getline(f, line)) {
    auto hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    std::istringstream in(line);
    Venue v;
    std::string mode;
    int oct = 0;
    if (!(in >> v.id >> mode >> v.base >> oct >> v.logo)) continue;
    std::getline(in >> std::ws, v.name);
    while (!v.name.empty() && (v.name.back() == '\r' || v.name.back() == ' ')) v.name.pop_back();
    v.pride = mode == "pride";
    v.octagon = oct != 0;
    if (v.id < 1 || v.id > 99 || v.name.empty() || (v.pride != (OctOf(v.base) == 2))) {
      REXLOG_WARN("[venues] bad line in venues.txt: '{}'", line);
      continue;
    }
    if (v.octagon && !std::filesystem::exists(dir / OctFile(v))) {
      REXLOG_WARN("[venues] {}: {} missing, using the stock cage/ring", v.name, OctFile(v));
      v.octagon = false;
    }
    if (v.logo && !std::filesystem::exists(dir / LogoFile(v))) {
      REXLOG_WARN("[venues] {}: {} missing, using the stock mat logos", v.name, LogoFile(v));
      v.logo = 0;
    }
    if (v.name.size() > 31) v.name.resize(31);
    for (int k = 0; k < 2; ++k) v.art[k] = std::filesystem::exists(dir / ArtFile(v, k));
    REXLOG_INFO("[venues] {} = stage {} ({}){}{}{}{}", v.name, v.base, v.pride ? "PRIDE" : "UFC",
                v.octagon ? " + " + OctFile(v) : "", v.logo ? " + " + LogoFile(v) : "",
                v.art[0] ? " + " + ArtFile(v, 0) : "", v.art[1] ? " + " + ArtFile(v, 1) : "");
    list.push_back(v);
  }
  return list;
}

// The venue screen in use and its virtual list.
struct Entry {
  int slot = 0;     // stock slot whose pictures (and, for stock entries, name) it shows
  int venue = -1;   // index into Venues(), -1 = a stock entry
};

struct Screen {
  uint32_t m = 0;   // guest object, 0 = none
  bool pride = false;
  std::vector<Entry> list;
  int v = 0;        // virtual cursor
  // rolling window
  bool laid_out = false;
  uint8_t names[kSlots * kNameBytes];
  uint8_t stock_names[kSlots * kNameBytes];   // the screen's own names, as Enter found them
  bool saved_names = false;
  uint32_t count = 0, sel = 0;
  // inside the input handler
  bool in_input = false;
  int g_in = 0, v_in = 0;
};

inline Screen g_screen;
inline std::atomic<uint64_t> g_pride_input_ms{0};   // last PRIDE-list input call (GetTickCount64)
inline int g_confirm_depth = 0;
inline bool g_pending_set = false;
inline int g_pending = -1;   // venue picked by the confirm in progress
inline int g_active = -1;    // venue whose cage/ring is loaded for its base stage

inline bool Trace() { return REXCVAR_GET(ufc3_trace_venues); }

inline int Mod(int a, int n) { return ((a % n) + n) % n; }
inline int Norm9(int d) {   // slot delta -> -4..4
  d = Mod(d, kSlots);
  return d > 4 ? d - kSlots : d;
}

inline bool Ours(uint32_t m) { return m && g_screen.m == m && !g_screen.list.empty(); }

inline int CurV(uint8_t* base) {
  Screen& s = g_screen;
  if (!s.in_input) return s.v;
  int g = int(Load32(base, s.m + kSel));
  return Mod(s.v_in + Norm9(g - s.g_in), int(s.list.size()));
}

inline void PutName(uint8_t* base, uint32_t dst, const std::string& name) {
  std::memset(base + dst, 0, kNameBytes);
  for (size_t i = 0; i < name.size() && i < 31; ++i) base[dst + i * 2 + 1] = uint8_t(name[i]);
}

// Fill the 9 name slots so that slot (g+d) names virtual entry (v+d), d = -4..4. Thumbnails and pictures
// are not copied: the screen's elements keep the ADDRESS of a record (FINDINGS-venue-pictures.md, proven
// by the vpic1 trace), so the vt+232 hook below hands each element the right record's address instead.
inline void Layout(uint8_t* base, int g, int v) {
  Screen& s = g_screen;
  if (!s.laid_out) {
    std::memcpy(s.names, base + s.m + kNames, sizeof s.names);
    s.count = Load32(base, s.m + kCount);
    s.sel = Load32(base, s.m + kSel);
    s.laid_out = true;
  }
  int n = int(s.list.size());
  for (int d = -4; d <= 4; ++d) {
    int slot = Mod(g + d, kSlots);
    const Entry& e = s.list[Mod(v + d, n)];
    uint32_t m = s.m;
    if (e.venue < 0)
      std::memcpy(base + m + kNames + slot * kNameBytes, s.stock_names + e.slot * kNameBytes, kNameBytes);
    else
      PutName(base, m + kNames + slot * kNameBytes, Venues()[e.venue].name);
  }
  Store32(base, s.m + kCount, kSlots);
}

inline void Restore(uint8_t* base) {
  Screen& s = g_screen;
  if (!s.laid_out) return;
  std::memcpy(base + s.m + kNames, s.names, sizeof s.names);
  Store32(base, s.m + kCount, s.count);
  s.laid_out = false;
}

// Build the virtual list after the screen filled its own.
inline void Enter(uint8_t* base, uint32_t m) {
  Screen& s = g_screen;
  s = Screen{};
  g_pride_input_ms.store(0);
  const auto& venues = Venues();
  if (venues.empty()) return;
  uint32_t count = Load32(base, m + kCount);
  if (count != 9 && count != 1) return;   // 9 = UFC list, 1 = PRIDE (Saitama only)
  s.m = m;
  s.pride = count == 1;
  std::memcpy(s.stock_names, base + m + kNames, sizeof s.stock_names);
  s.saved_names = true;
  for (uint32_t i = 0; i < count; ++i) s.list.push_back({int(i), -1});
  for (int i = 0; i < int(venues.size()); ++i) {
    const Venue& v = venues[i];
    if (v.pride != s.pride) continue;
    int slot = -1;
    if (s.pride) {
      slot = 0;
    } else {
      for (int k = 1; k < 9; ++k)
        if (int(Load32(base, kUfcTable + (k - 1) * 4)) == v.base) slot = k;
    }
    if (slot < 0) {
      REXLOG_WARN("[venues] {}: stage {} is not on the UFC venue list", v.name, v.base);
      continue;
    }
    s.list.push_back({slot, i});
  }
  if (int(s.list.size()) == int(count)) {   // nothing for this list
    s = Screen{};
    return;
  }
  s.v = s.pride ? 0 : int(Load32(base, m + kSel));
  if (s.v >= int(s.list.size())) s.v = 0;
  // Coming back to the screen after picking an extra venue: point at it.
  if (g_active >= 0) {
    for (int k = 0; k < int(s.list.size()); ++k)
      if (s.list[k].venue == g_active && s.list[k].slot == int(Load32(base, m + kSel)) ) s.v = k;
  }
  if (Trace()) REXLOG_INFO("[venues] screen {:08X}: {} entries ({}), cursor {}", m, s.list.size(),
                           s.pride ? "PRIDE" : "UFC", s.v);
}

// Guest copies of a venue's two file-name formats (cage/ring, logos), made once.
inline uint32_t GuestFormats(uint8_t* base, int venue) {
  static std::vector<uint32_t> made;
  if (made.size() < Venues().size()) made.resize(Venues().size(), 0);
  if (!made[venue]) {
    const Venue& v = Venues()[venue];
    uint32_t g = REX_KERNEL_MEMORY()->SystemHeapAlloc(2 * kFmtRoom);
    if (!g) return 0;
    std::memset(base + g, 0, 2 * kFmtRoom);
    std::snprintf(reinterpret_cast<char*>(base + g), kFmtRoom, "stage/octagon_%%02d_v%02d.pac", v.id);
    std::snprintf(reinterpret_cast<char*>(base + g + kFmtRoom), kFmtRoom, "stage/%%04d_%02d_octlogo.tex", v.logo);
    made[venue] = g;
  }
  return made[venue];
}

// Writes the entry under the cursor into the name slot the screen shows (kSel's slot) and keeps
// it there: the screen reads that text after the handler returns, so restoring the stock name
// at once showed SAITAMA SUPER ARENA for every PRIDE entry.
inline void ShowCurrentName(uint8_t* base) {
  Screen& s = g_screen;
  if (!Ours(s.m) || s.laid_out) return;
  if (s.saved_names) std::memcpy(base + s.m + kNames, s.stock_names, sizeof s.stock_names);
  const Entry& e = s.list[s.v];
  uint32_t slot = s.pride ? 0 : uint32_t(e.slot);
  uint32_t dst = s.m + kNames + slot * kNameBytes;
  if (e.venue >= 0)
    PutName(base, dst, Venues()[e.venue].name);
  else if (s.saved_names)
    std::memcpy(base + dst, s.stock_names + e.slot * kNameBytes, kNameBytes);
}

// ---- own thumbnail + picture per extra venue ----
// Each venue with art gets two records of its own ({u32 handle, u32 texture}, as the screen's), made once
// per session; the textures are loaded when the screen's loader finishes its own (sub_8299D070 phase 2 -> 3)
// and destroyed with the screen (sub_8299CE00), the same calls as the stock ones.
constexpr uint32_t kHandleTable = 0x833C82FC;   // [ ] = texture handle table (sub_8277E068)
constexpr uint32_t kTexReleaser = 0x833C8104;   // [ ] = deferred texture destroy queue (sub_82748FE0)
struct OwnArt {
  uint32_t rec[2] = {0, 0};   // guest records: thumb, picture
  uint32_t buf[2] = {0, 0};   // guest copies of the DDS files (kept until teardown)
  bool loaded[2] = {false, false};
};
inline std::vector<OwnArt> g_art;
inline uint32_t g_art_owner = 0;   // screen whose loader filled them
inline bool g_redirect = false;    // inside our sub_8299B5F0 call
inline bool g_hold_referee = false;  // PRIDE list input in the referee column: no referee scroll

inline void MakeRecords(uint8_t* base) {
  const auto& venues = Venues();
  if (g_art.size() < venues.size()) g_art.resize(venues.size());
  for (size_t i = 0; i < venues.size(); ++i)
    for (int k = 0; k < 2; ++k)
      if (venues[i].art[k] && !g_art[i].rec[k]) {
        uint32_t r = REX_KERNEL_MEMORY()->SystemHeapAlloc(kRecBytes);
        if (!r) continue;
        Store32(base, r, 0xFFFF);
        Store32(base, r + 4, 0);
        g_art[i].rec[k] = r;
      }
}

inline uint32_t OwnRecord(int venue, bool pic) {
  if (venue < 0 || venue >= int(g_art.size())) return 0;
  return g_art[venue].rec[pic ? 1 : 0];
}

}  // namespace ufc3_venues

extern "C" REX_FUNC(sub_8299C678) {
  uint32_t m = ctx.r3.u32;
  if (ufc3_venues::Trace())
    REXLOG_INFO("[venues] pics enter {:08X} phase={} count={}", m, ufc3_venues::Load32(base, m + 208),
                ufc3_venues::Load32(base, m + ufc3_venues::kCount));
  __imp__sub_8299C678(ctx, base);
  ufc3_venues::Enter(base, m);
  ufc3_venues::ShowCurrentName(base);
  if (ufc3_venues::Ours(m)) {
    // The screen drew its strip before our list existed: draw it again so every element holds the
    // address of the record its entry shows (the cursor may sit on an extra venue).
    ufc3_venues::MakeRecords(base);
    PPCContext c = ctx;
    c.r3.u64 = m;
    sub_8299B5F0(c, base);
  }
}

// --ufc3_trace_venues: every element id of the screen's scene (layer 0, object 0, as sub_828F84C8 reads
// it: [scene+36] -> [+16][0] = obj; [obj+16]+4 = id table {+24 count, +28 {u32 n, u16* nodes}[]};
// nodes at [obj+12], 316 bytes each), with each node's bytes, once per screen and again in the referee column.
inline void TraceSceneNodes(uint8_t* base, uint32_t m) {
  using namespace ufc3_venues;
  static uint32_t done_for = 0, done_col1 = 0, col1_frames = 0;
  col1_frames = Load32(base, m + 220) == 1 ? col1_frames + 1 : 0;
  bool col1 = col1_frames >= 40;   // referee column, its switch animation done
  if (done_for == m && (!col1 || done_col1 == m)) return;
  if (col1) done_col1 = m;
  done_for = m;
  uint32_t scene = Load32(base, m + 172);
  uint32_t layer = scene ? Load32(base, scene + 36) : 0;
  uint32_t objs = layer ? Load32(base, layer + 16) : 0;
  uint32_t obj = (layer && Load32(base, layer + 12) > 0 && objs) ? Load32(base, objs) : 0;
  uint32_t t1 = obj ? Load32(base, obj + 16) : 0;
  uint32_t t = t1 ? Load32(base, t1 + 4) : 0;
  uint32_t nodes = obj ? Load32(base, obj + 12) : 0;
  if (!t || !nodes) {
    REXLOG_INFO("[venues] scene {:08X}: no id table", m);
    return;
  }
  uint32_t n = Load32(base, t + 24), arr = Load32(base, t + 28);
  REXLOG_INFO("[venues] scene {:08X} obj {:08X}: {} ids, nodes {:08X}", m, obj, n, nodes);
  // every node by index (each starts with its object's address), whatever its id
  for (uint32_t i = 0; i < 1024 && Load32(base, nodes + i * 316) == obj; ++i) {
    std::string hex;
    char b[12];
    for (uint32_t o = 0; o < 316; o += 4) {
      std::snprintf(b, sizeof b, "%08X ", Load32(base, nodes + i * 316 + o));
      hex += b;
    }
    REXLOG_INFO("[venues] node idx {} col {}: {}", i, Load32(base, m + 220), hex);
  }
  for (uint32_t id = 0; id < n && id < 4096; ++id) {
    uint32_t cnt = Load32(base, arr + 8 * id), list = Load32(base, arr + 8 * id + 4);
    for (uint32_t j = 0; j < cnt && j < 8; ++j) {
      int16_t idx = int16_t((base[list + 2 * j] << 8) | base[list + 2 * j + 1]);
      uint32_t node = nodes + uint32_t(idx) * 316;
      std::string hex;
      char b[12];
      for (uint32_t o = 0; o < 316; o += 4) {
        std::snprintf(b, sizeof b, "%08X ", Load32(base, node + o));
        hex += b;
      }
      REXLOG_INFO("[venues] node id {} #{} idx {} @{:08X}: {}", id, j, idx, node, hex);
    }
  }
}

// Venue screen state for --ufc3_trace_venues: logged when it changes.
inline void TraceInput(uint8_t* base, uint32_t m, const char* when) {
  using namespace ufc3_venues;
  static uint32_t last[6] = {};
  uint32_t now[6] = {m, Load32(base, m + 136), Load32(base, m + 220), Load32(base, m + kCount),
                     Load32(base, m + kSel), Load32(base, m + 1544)};
  if (std::memcmp(now, last, sizeof now) == 0) return;
  std::memcpy(last, now, sizeof now);
  REXLOG_INFO("[venues] input {} {:08X}: +136={} column={} count={} sel={} anim={} ours={}", when, m,
              now[1], now[2], now[3], now[4], int32_t(now[5]), Ours(m));
}

inline void TracePicsAfterInput(uint8_t* base, uint32_t m);

extern "C" REX_FUNC(sub_8299B9C0) {
  using namespace ufc3_venues;
  uint32_t m = ctx.r3.u32;
  if (Trace()) TraceInput(base, m, "pre");
  if (Trace() && Load32(base, m + 208) == 4) TraceSceneNodes(base, m);
  if (!Ours(m)) return __imp__sub_8299B9C0(ctx, base);
  Screen& s = g_screen;
  int g0 = int(Load32(base, m + kSel));
  Layout(base, g0, s.v);
  s.in_input = true;
  s.g_in = g0;
  s.v_in = s.v;
  // PRIDE: the stock list code instead of the handler's "fixed in PRIDE" exit (see the top).
  uint32_t mode = 0, mode_vt = 0;
  if (s.pride) {
    g_pride_input_ms.store(GetTickCount64());
    mode = Load32(base, kModePtr);
    mode_vt = mode ? Load32(base, mode) : 0;
    uint32_t fake = mode_vt ? FakeModeVtable(base, mode_vt) : 0;
    if (fake) Store32(base, mode, fake);
    else mode = 0;
  }
  // PRIDE keeps its referee: a step in the referee column is taken back (referee and its
  // animation counter as they were, so the end-of-step redraw never runs) and its strip scroll
  // (scr_up_r / scr_dwn_r) is not played: started and never reset, it left a second referee
  // thumbnail standing above the referee icon.
  const bool referee_column = mode && Load32(base, m + 220) != 0;
  const uint32_t referee = Load32(base, m + 1464), anim = Load32(base, m + 1544);
  g_hold_referee = referee_column;
  __imp__sub_8299B9C0(ctx, base);
  g_hold_referee = false;
  if (referee_column && Load32(base, m + 1464) != referee) {
    Store32(base, m + 1464, referee);
    Store32(base, m + 1544, anim);
    if (Trace()) REXLOG_INFO("[venues] PRIDE list: referee step taken back");
  }
  if (mode && Load32(base, mode) != mode_vt) Store32(base, mode, mode_vt);
  if (!Ours(m)) return;   // the screen went away inside
  int g1 = int(Load32(base, m + kSel));
  s.v = Mod(s.v + Norm9(g1 - g0), int(s.list.size()));
  s.in_input = false;
  Restore(base);
  Store32(base, m + kSel, s.pride ? 0 : uint32_t(s.list[s.v].slot));
  ShowCurrentName(base);
  if (g1 != g0 && Trace()) {
    TracePicsAfterInput(base, m);
    const Entry& e = s.list[s.v];
    REXLOG_INFO("[venues] cursor -> {} ({})", s.v,
                e.venue >= 0 ? Venues()[e.venue].name : "stock slot " + std::to_string(e.slot));
  }
}

extern "C" REX_FUNC(sub_8299B5F0) {
  using namespace ufc3_venues;
  uint32_t m = ctx.r3.u32;
  if (!Ours(m)) return __imp__sub_8299B5F0(ctx, base);
  Screen& s = g_screen;
  if (s.in_input) {   // called by the input handler: follow its cursor
    int g = int(Load32(base, m + kSel));
    Layout(base, g, CurV(base));
    g_redirect = true;
    __imp__sub_8299B5F0(ctx, base);
    g_redirect = false;
    return;
  }
  Layout(base, int(Load32(base, m + kSel)), s.v);
  g_redirect = true;
  __imp__sub_8299B5F0(ctx, base);
  g_redirect = false;
  Restore(base);
}

extern "C" REX_FUNC(sub_8299C188) {
  using namespace ufc3_venues;
  uint32_t m = ctx.r3.u32;
  __imp__sub_8299C188(ctx, base);
  if (Ours(m) && !g_screen.in_input) {
    g_screen.v = g_screen.pride ? 0 : int(Load32(base, m + kSel));
    if (g_screen.v >= int(g_screen.list.size())) g_screen.v = 0;
    ShowCurrentName(base);
    PPCContext c = ctx;
    c.r3.u64 = m;
    sub_8299B5F0(c, base);
  }
}

// The stage of the cursor (and "is it Any?") as the stock list would answer
// for the entry's base slot; a confirm in progress notes the extra venue.
inline void WithStockCursor(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), bool note) {
  using namespace ufc3_venues;
  uint32_t m = ctx.r3.u32;
  if (!Ours(m)) return fn(ctx, base);
  const Entry& e = g_screen.list[CurV(base)];
  uint32_t sel = Load32(base, m + kSel);
  Store32(base, m + kSel, g_screen.pride ? 0 : uint32_t(e.slot));
  fn(ctx, base);
  Store32(base, m + kSel, sel);
  if (note && g_confirm_depth > 0) {
    g_pending = e.venue;
    g_pending_set = true;
  }
}

extern "C" REX_FUNC(sub_8299C410) { WithStockCursor(ctx, base, __imp__sub_8299C410, true); }
extern "C" REX_FUNC(sub_8299B8C8) { WithStockCursor(ctx, base, __imp__sub_8299B8C8, false); }

inline void VenueConfirm(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*)) {
  using namespace ufc3_venues;
  ++g_confirm_depth;
  g_pending_set = false;
  fn(ctx, base);
  --g_confirm_depth;
  g_pending_set = false;
}

extern "C" REX_FUNC(sub_829C3180) { VenueConfirm(ctx, base, __imp__sub_829C3180); }
extern "C" REX_FUNC(sub_82A76CD8) { VenueConfirm(ctx, base, __imp__sub_82A76CD8); }
extern "C" REX_FUNC(sub_82A1DA50) { VenueConfirm(ctx, base, __imp__sub_82A1DA50); }

extern "C" REX_FUNC(sub_82ED9C08) {
  using namespace ufc3_venues;
  int stage = ctx.r4.s32;
  const auto& venues = Venues();
  if (!venues.empty()) {
    int before = g_active;
    if (g_confirm_depth > 0 && g_pending_set) {
      g_active = (g_pending >= 0 && venues[g_pending].base == stage) ? g_pending : -1;
      g_pending_set = false;
    } else if (g_active >= 0 && stage >= 0 && stage != venues[g_active].base) {
      g_active = -1;
    }
    if (Trace())
      REXLOG_INFO("[venues] SetStage({}, {}, {}) lr={:08X} confirm={} active: {} -> {}", stage,
                  ctx.r5.s32, ctx.r6.s32, uint32_t(ctx.lr), g_confirm_depth,
                  before >= 0 ? venues[before].name : "-", g_active >= 0 ? venues[g_active].name : "-");
  }
  __imp__sub_82ED9C08(ctx, base);
}

// ---- venue screen pictures: own records (load, element redirect, teardown) + trace ----
REX_EXTERN(__imp__sub_8299D070);  // venue screen: per-frame loader (vt+16)
REX_EXTERN(__imp__sub_8299CE00);  // venue screen: teardown (dtor body)
REX_EXTERN(__imp__sub_828F7C58);  // base screen vt+232: element <- texture record address
REX_EXTERN(__imp__sub_828F7930);  // base screen vt+120: play a scene animation by name
REX_EXTERN(sub_82757F60);  // new texture object (96 bytes)
REX_EXTERN(sub_8277E068);  // record handle for a texture (out, handle table, texture)
REX_EXTERN(sub_8279C6C8);  // current texture memory pool
REX_EXTERN(sub_8279D358);  // set texture memory pool
REX_EXTERN(sub_82748FE0);  // deferred texture destroy (queue, texture)
REX_EXTERN(sub_822841B0);  // release a record's handle

namespace ufc3_venues {
constexpr uint32_t kScreenVt = 0x82107DC0;
constexpr uint32_t kPhase = 208;
inline bool IsVenueScreen(uint8_t* base, uint32_t m) { return m && Load32(base, m) == kScreenVt; }
inline uint32_t g_elem_addr[512] = {};   // trace: last record address per element id
inline std::string CStr(uint8_t* base, uint32_t a, size_t max = 48) {
  std::string s;
  for (size_t i = 0; a && i < max && base[a + i]; ++i) s += char(base[a + i]);
  return s;
}
inline const char* RecKind(uint32_t m, uint32_t r, int& k) {
  if (r >= m + kThumbs && r < m + kThumbs + kSlots * kRecBytes) { k = int((r - m - kThumbs) / kRecBytes); return "thumb"; }
  if (r >= m + kPics && r < m + kPics + kSlots * kRecBytes) { k = int((r - m - kPics) / kRecBytes); return "pic"; }
  if (r >= m + 1464 && r < m + 1464 + 9 * kRecBytes) { k = int((r - m - 1464) / kRecBytes); return "ref"; }
  k = -1;
  return "other";
}
}  // namespace ufc3_venues

inline void TracePicsAfterInput(uint8_t* base, uint32_t m) {
  using namespace ufc3_venues;
  for (uint32_t e : {451u, 459u, 460u}) {
    uint32_t a = g_elem_addr[e];
    if (a) REXLOG_INFO("[venues] pics after input sel={} elem {} @{:08X} now = {:08X} {:08X}", Load32(base, m + kSel), e, a,
                       Load32(base, a), Load32(base, a + 4));
  }
}

namespace ufc3_venues {
// vt+12 of the texture object (sub_82785F60): texture from DDS bytes in memory (the stock loader's call).
inline void TexFromMemory(PPCContext& ctx, uint8_t* base, uint32_t tex, uint32_t data, uint32_t size) {
  PPCContext saved = ctx;
  ctx.r3.u64 = tex;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = data;
  ctx.r6.u64 = size;
  ctx.r7.u64 = 0;
  ctx.r8.u64 = 1;
  ctx.ctr.u64 = Load32(base, Load32(base, tex) + 12);
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
  ctx = saved;
}

// The extra venues' textures, loaded as the stock loader loads its own (sub_8299D070 phase 2).
inline void LoadArt(PPCContext& ctx, uint8_t* base, uint32_t m) {
  const auto& venues = Venues();
  MakeRecords(base);
  int done = 0;
  for (const Entry& e : g_screen.list) {
    if (e.venue < 0) continue;
    OwnArt& a = g_art[e.venue];
    for (int k = 0; k < 2; ++k) {
      if (!venues[e.venue].art[k] || !a.rec[k] || a.loaded[k]) continue;
      std::ifstream f(StageDir() / ArtFile(venues[e.venue], k), std::ios::binary);
      std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4) != 0) {
        REXLOG_WARN("[venues] {}: {} unreadable", venues[e.venue].name, ArtFile(venues[e.venue], k));
        continue;
      }
      uint32_t buf = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(bytes.size()));
      if (!buf) continue;
      std::memcpy(base + buf, bytes.data(), bytes.size());
      PPCContext c = ctx;
      c.r3.u64 = 0;
      sub_82757F60(c, base);   // new texture object
      uint32_t tex = c.r3.u32;
      if (!tex) {
        REX_KERNEL_MEMORY()->SystemHeapFree(buf);
        continue;
      }
      c = ctx;
      c.r3.u64 = a.rec[k];
      c.r4.u64 = Load32(base, kHandleTable);
      c.r5.u64 = tex;
      sub_8277E068(c, base);   // record handle
      Store32(base, a.rec[k] + 4, tex);
      c = ctx;
      sub_8279C6C8(c, base);
      uint32_t old = c.r3.u32;
      c = ctx;
      c.r3.u64 = 27;
      sub_8279D358(c, base);
      TexFromMemory(ctx, base, tex, buf, uint32_t(bytes.size()));
      c = ctx;
      c.r3.u64 = old;
      sub_8279D358(c, base);
      a.buf[k] = buf;
      a.loaded[k] = true;
      ++done;
      if (Trace())
        REXLOG_INFO("[venues] pics own {} {} @{:08X} = {:08X} {:08X} ({} bytes)", venues[e.venue].name,
                    k ? "picture" : "thumb", a.rec[k], Load32(base, a.rec[k]), tex, bytes.size());
    }
  }
  if (done) g_art_owner = m;
}

inline void FreeArt(PPCContext& ctx, uint8_t* base, uint32_t m) {
  if (!g_art_owner || g_art_owner != m) return;
  for (OwnArt& a : g_art)
    for (int k = 0; k < 2; ++k) {
      if (!a.loaded[k]) continue;
      PPCContext c = ctx;
      c.r3.u64 = Load32(base, kTexReleaser);
      c.r4.u64 = Load32(base, a.rec[k] + 4);
      sub_82748FE0(c, base);   // destroy once the GPU is done with it
      c = ctx;
      c.r3.u64 = a.rec[k];
      sub_822841B0(c, base);   // release the handle
      Store32(base, a.rec[k], 0xFFFF);
      Store32(base, a.rec[k] + 4, 0);
      if (a.buf[k]) REX_KERNEL_MEMORY()->SystemHeapFree(a.buf[k]);
      a.buf[k] = 0;
      a.loaded[k] = false;
    }
  g_art_owner = 0;
  if (Trace()) REXLOG_INFO("[venues] pics own textures released {:08X}", m);
}
}  // namespace ufc3_venues

extern "C" REX_FUNC(sub_8299D070) {
  using namespace ufc3_venues;
  uint32_t m = ctx.r3.u32;
  uint32_t a = Load32(base, m + kPhase);
  __imp__sub_8299D070(ctx, base);
  uint32_t b = Load32(base, m + kPhase);
  if (a == 2 && b == 3 && Ours(m)) LoadArt(ctx, base, m);
  if (!Trace() || a == b) return;
  REXLOG_INFO("[venues] pics phase {}->{} {:08X} count={} pkgs={:08X} {:08X} {:08X}", a, b, m,
              Load32(base, m + kCount), Load32(base, m + 1532), Load32(base, m + 1536), Load32(base, m + 1540));
  if (b == 3)
    for (int k = 0; k < kSlots; ++k) {
      uint32_t t = m + kThumbs + k * kRecBytes, p = m + kPics + k * kRecBytes;
      REXLOG_INFO("[venues] pics rec thumb[{}] @{:08X} = {:08X} {:08X}  pic[{}] @{:08X} = {:08X} {:08X}", k, t,
                  Load32(base, t), Load32(base, t + 4), k, p, Load32(base, p), Load32(base, p + 4));
    }
}

extern "C" REX_FUNC(sub_8299CE00) {
  using namespace ufc3_venues;
  if (Trace()) REXLOG_INFO("[venues] pics teardown {:08X}", ctx.r3.u32);
  FreeArt(ctx, base, ctx.r3.u32);
  __imp__sub_8299CE00(ctx, base);
}

// Base screen vt+232 (element <- record address), shared by every screen: while our sub_8299B5F0 call
// runs, a thumbnail/picture record of the venue screen is swapped for the one of the entry that element
// shows (the venue's own, or its base arena's stock record). The element keeps that address.
extern "C" REX_FUNC(sub_828F7C58) {
  using namespace ufc3_venues;
  uint32_t m = ctx.r3.u32;
  if (g_redirect && Ours(m)) {
    uint32_t r = ctx.r5.u32;
    bool thumb = r >= m + kThumbs && r < m + kThumbs + kSlots * kRecBytes;
    bool pic = r >= m + kPics && r < m + kPics + kSlots * kRecBytes;
    if (thumb || pic) {
      int k = int((r - m - (pic ? kPics : kThumbs)) / kRecBytes);
      const Screen& s = g_screen;
      const Entry& e = s.list[Mod(CurV(base) + Norm9(k - int(Load32(base, m + kSel))), int(s.list.size()))];
      uint32_t own = e.venue >= 0 ? OwnRecord(e.venue, pic) : 0;
      ctx.r5.u64 = own ? own : m + (pic ? kPics : kThumbs) + uint32_t(s.pride ? 0 : e.slot) * kRecBytes;
    }
  }
  if (Trace() && IsVenueScreen(base, m)) {
    uint32_t e = ctx.r4.u32, r = ctx.r5.u32;
    int k;
    const char* kind = RecKind(m, r, k);
    if (e < 512) g_elem_addr[e] = r;
    REXLOG_INFO("[venues] pics elem {} <- {:08X} ({} {}) = {:08X} {:08X}", e, r, kind, k, Load32(base, r),
                Load32(base, r + 4));
  }
  __imp__sub_828F7C58(ctx, base);
}

extern "C" REX_FUNC(sub_828F7930) {
  using namespace ufc3_venues;
  if (!IsVenueScreen(base, ctx.r3.u32)) return __imp__sub_828F7930(ctx, base);
  std::string name = CStr(base, ctx.r4.u32);
  bool skip = g_hold_referee && (name == "scr_up_r" || name == "scr_dwn_r");
  if (Trace()) REXLOG_INFO("[venues] anim '{}' r6={} r7={}{}", name, ctx.r6.s32, ctx.r7.s32, skip ? " (held)" : "");
  if (skip) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_828F7930(ctx, base);
}

extern "C" REX_FUNC(sub_82C42DB0) {
  using namespace ufc3_venues;
  int oct = ctx.r4.s32;
  int stage = int(Load32(base, kStage));
  if (g_active < 0 || Venues()[g_active].base != stage) {
    if (Trace()) REXLOG_INFO("[venues] cage/ring load: stage {} oct {} (stock)", stage, oct);
    return __imp__sub_82C42DB0(ctx, base);
  }
  const Venue& v = Venues()[g_active];
  bool po = v.octagon && oct == OctOf(v.base);
  bool pl = v.logo != 0;
  uint32_t fmts = (po || pl) ? GuestFormats(base, g_active) : 0;
  if (!fmts) po = pl = false;
  uint32_t saved[3] = {Load32(base, kFmtTable), Load32(base, kFmtTable + 4), Load32(base, kFmtTable + 8)};
  if (po) {
    Store32(base, kFmtTable, fmts);
    Store32(base, kFmtTable + 4, fmts);
  }
  if (pl) Store32(base, kFmtTable + 8, fmts + kFmtRoom);
  if (Trace())
    REXLOG_INFO("[venues] cage/ring load: stage {} oct {} -> {}{}{}", stage, oct, v.name,
                po ? " " + OctFile(v) : "", pl ? " " + LogoFile(v) : "");
  __imp__sub_82C42DB0(ctx, base);
  for (int i = 0; i < 3; ++i) Store32(base, kFmtTable + 4 * i, saved[i]);
}
