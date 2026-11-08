// ufc3 - attire picker on SELECT FIGHTER (FINDINGS-attire.md section 4).
//
// A fighter is loaded as code ID*100 + attire (0 = main, trunks_c; 1 = alternate,
// trunks_c2). sub_829A6EB0(select, side) decides the attire whenever a cursor
// lands on a fighter (called from sub_829A8EE8, which then loads the 3D preview
// via sub_8290A770 and the 2D render with that attire) and stores it at
// side+68; the match uses the same value. Stock exhibition only gives the
// alternate to the second fighter of a mirror match.
//
// In game: clicking the left stick (L3) flips the attire main <-> alt of the
// side whose cursor moved last. The choice is kept per side and fighter for the
// rest of the session, and sub_829A6EB0 applies it on top of the stock rule,
// so the fight (and a mirror match's other side) keeps it. The press is only
// queued by the pad hook (in bare_knuckle.h, which owns XamInputGetState); the
// select screen's own frame update (sub_829ABD80, vtable 0x82108DB0 slot 4)
// does the flip by calling sub_829A8EE8 again, so the 3D preview and 2D render
// reload at once. While that side's 3D preview is still loading, the press
// waits (and further presses are ignored). Online/lobby modes (+3504 = 3/4/5)
// keep the stock rule. Ports whose trunks_c2 is a copy of trunks_c show no change.
//
// More than two attires: <mods>/DAT/chara/attire_counts.txt lists "<id> <count>". A listed
// fighter cycles 0..count-1 on L3. The game itself only ever sees attire 0 or 1: a real attire 2
// in the fighter code crashed the walkout gear load at fight start (memcpy to NULL in sub_827A49E0,
// att3b runs). So side+68 and the code stay 0/1 and g_slot says which real attire each of the two
// stands for, per model id. Two different real attires in a mirror match take one engine value
// each. The loader (sub_82BD08A8) is steered by three mid-function hooks, stateless per call:
// it loads the real attire's _c/_c_b under the stock names and skips all the others, so memory
// stays one set. Unlisted fighters take the stock path untouched.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/filesystem.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

#include "mod_pack.h"

REXCVAR_DEFINE_BOOL(ufc3_attire_trace, false, "ufc3", "Log attire picker calls ([attire] lines; debug)");
REXCVAR_DEFINE_BOOL(ufc3_attire_picker, true, "ufc3",
                    "L3 on SELECT FIGHTER switches attire (needs the mod pack marker; off without it whatever this says)");
REXCVAR_DEFINE_BOOL(ufc3_select_hints, true, "ufc3", "SELECT FIGHTER button legend also lists R3 bare knuckle (and L3 attire with ufc3_attire_picker)");

namespace ufc3_attire {

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}
inline void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = _byteswap_ulong(v);
  std::memcpy(base + addr, &v, 4);
}

// Select-screen layout (sub_829A6EB0 / sub_829A8EE8 / sub_8290A770).
constexpr uint32_t kSideObj[2] = {3448, 3452};  // side objects: +20 fighter index, +68 attire
constexpr uint32_t kMode = 3504;                // 3/4/5 = online/lobby modes
constexpr uint32_t kPreview = 4536;             // 3D preview manager; per side +side*96+52 = load state

inline bool OnlineMode(uint8_t* base, uint32_t select) {
  uint32_t m = Load32(base, select + kMode);
  return m == 3 || m == 4 || m == 5;
}

inline std::mutex g_mutex;
inline std::unordered_map<uint32_t, uint32_t> g_pick[2];  // fighter index -> attire, per side

// <mods>/DAT/chara/attire_counts.txt, one line per fighter: "<id> <count> [<colour hash> ...]".
// The hashes (hex, generic.ytx group 0x050C5D78: BLACK 17D6BE4C, OLIVE 17D6BE44...) name the
// trunks colour of attires 2, 3... for the fight caption; attires 0/1 keep the record's own.
struct AttireInfo {
  int count = 2;
  std::vector<uint32_t> colour;  // per extra attire (index 0 = attire 2)
};
inline const std::unordered_map<uint32_t, AttireInfo>& AttireTable() {
  static const std::unordered_map<uint32_t, AttireInfo> table = [] {
    std::unordered_map<uint32_t, AttireInfo> m;
    if (!ufc3_pack::Active()) return m;  // stock mode: two attires, as on the disc
    std::string mods = rex::cvar::GetFlagByName("mods_root");
    if (mods.empty()) return m;
    std::ifstream f(rex::to_path(mods) / "DAT" / "chara" / "attire_counts.txt");
    std::string line;
    while (std::getline(f, line)) {
      std::istringstream in(line);
      uint32_t id;
      int n;
      if (!(in >> id >> n) || n < 3 || n > 9) continue;
      AttireInfo a;
      a.count = n;
      std::string h;
      while (in >> h) a.colour.push_back(static_cast<uint32_t>(std::stoul(h, nullptr, 16)));
      m[id] = a;
    }
    REXLOG_INFO("[attire] {} fighters with more than 2 attires", m.size());
    return m;
  }();
  return table;
}
inline int AttireCount(uint32_t id) {
  auto it = AttireTable().find(id);
  return it == AttireTable().end() ? 2 : it->second.count;
}
inline uint32_t ExtraColour(uint32_t id, uint32_t real) {  // 0 = not given
  auto it = AttireTable().find(id);
  if (it == AttireTable().end() || real < 2 || real - 2 >= it->second.colour.size()) return 0;
  return it->second.colour[real - 2];
}

// Last cursor landing per side: the select object and the grid entry sub_829A8EE8 got.
struct Landing {
  uint32_t select, entry;
};
inline Landing g_landing[2] = {};
inline std::atomic<int> g_last_side{0};
inline std::atomic<uint32_t> g_side_id[2] = {};  // the model id the 3D preview last got (+20 of the side object is a list index)
inline std::atomic<uint32_t> g_real[2] = {};     // the real attire on each side
struct Slot {
  uint8_t real[2] = {0, 1};  // engine attire -> real attire
};
inline std::unordered_map<uint32_t, Slot> g_slot;  // by model id, under g_mutex
inline uint32_t RealAttire(uint32_t id, uint32_t engine) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_slot.find(id);
  return it == g_slot.end() ? engine : it->second.real[engine & 1];
}

inline std::atomic<uint64_t> g_select_tick{0};  // last frame of the select screen
inline std::atomic<bool> g_select_online{false};  // that screen is an online/lobby one
inline void (*g_on_preview_ready)(PPCContext& ctx, uint8_t* base, int side) = nullptr;  // bare_knuckle.h: re-hide gloves
inline uint32_t g_prev_state[2] = {0, 0};
inline std::atomic<int> g_pending{0};           // L3 presses not applied yet (applied together once the preview is idle)

// Pad hook (bare_knuckle.h, XamInputGetState): queue an L3 press while the
// select screen runs.
inline void OnPad(uint32_t user, uint16_t buttons) {
  if (!REXCVAR_GET(ufc3_attire_picker) || !ufc3_pack::Active()) return;
  static bool held[4] = {};
  bool l3 = (buttons & 0x0040) != 0;  // XINPUT_GAMEPAD_LEFT_THUMB
  bool pressed = l3 && !held[user & 3];
  held[user & 3] = l3;
  if (!pressed) return;
  if (GetTickCount64() - g_select_tick.load(std::memory_order_relaxed) > 500) return;
  int n = ++g_pending;
  if (REXCVAR_GET(ufc3_attire_trace)) REXLOG_INFO("[attire] L3 (pad {}), side {}, {} queued", user, g_last_side.load(), n);
}

}  // namespace ufc3_attire

REX_EXTERN(__imp__sub_829A6EB0);
REX_EXTERN(__imp__sub_829A8EE8);
REX_EXTERN(__imp__sub_829ABD80);
REX_EXTERN(__imp__sub_8290A770);

// Fight caption ("Trunks  VELASQUEZ OLIVE"): the fighter record's gear block, 24 bytes per attire at +0x80,
// whose first three words are the trunks colour names. With an extra attire behind an engine value the
// record's words for that value are swapped for the real attire's while it is picked. The record lives
// in the table legacy_roster.h builds; its save copy-back runs between CaptionsForSave(false) and (true),
// so the save never keeps a swapped caption.
namespace ufc3_attire {
constexpr uint32_t kFighterDb = 0x83576FC8, kTableHeader = 16, kRecordSize = 276, kGear = 0x80;
struct Caption {
  uint32_t rec = 0;
  uint32_t orig[2][3] = {};
  uint32_t want[2][3] = {};
  bool on = false;
};
inline std::unordered_map<uint32_t, Caption> g_caption;  // by model id, under g_mutex
inline uint32_t FindRecord(uint8_t* base, uint32_t id) {
  uint32_t db = Load32(base, kFighterDb);
  if (!db) return 0;
  uint32_t table = Load32(base, db);
  if (!table) return 0;
  uint32_t n = Load32(base, table);
  for (uint32_t i = 0; i < n && i < 2048; ++i) {
    uint32_t rec = table + kTableHeader + i * kRecordSize;
    if ((Load32(base, rec) >> 16) == id) return rec;
  }
  return 0;
}
inline void WriteCaption(uint8_t* base, const Caption& c, bool want) {
  for (int e = 0; e < 2; ++e)
    for (int w = 0; w < 3; ++w) Store32(base, c.rec + kGear + 24 * e + 4 * w, want ? c.want[e][w] : c.orig[e][w]);
}
// Under g_mutex: the slot of model `id` changed.
inline void UpdateCaption(uint8_t* base, uint32_t id, const Slot& slot) {
  uint32_t rec = FindRecord(base, id);
  if (!rec) return;
  Caption& c = g_caption[id];
  if (c.rec != rec || !c.on) {  // first time, or the table was rebuilt: take the record as it is now
    c.rec = rec;
    for (int e = 0; e < 2; ++e)
      for (int w = 0; w < 3; ++w) c.orig[e][w] = Load32(base, rec + kGear + 24 * e + 4 * w);
  }
  for (int e = 0; e < 2; ++e) {
    uint32_t real = slot.real[e];
    for (int w = 0; w < 3; ++w) c.want[e][w] = real < 2 ? c.orig[real][w] : 0;
    if (real >= 2) {
      uint32_t colour = ExtraColour(id, real);
      c.want[e][0] = colour ? colour : c.orig[e][0];
    }
  }
  c.on = true;
  WriteCaption(base, c, true);
  if (REXCVAR_GET(ufc3_attire_trace))
    REXLOG_INFO("[attire] caption id {} rec {:08X}: engine 0 = {:08X}, engine 1 = {:08X}", id, rec, c.want[0][0], c.want[1][0]);
}
// A select screen outside exhibition (career, event, title...: select flag +4516 = 0 gives attire 0 by the
// stock rule) must not inherit an extra attire: every slot back to engine = real, captions back.
inline void ResetSlots(uint8_t* base, const char* why) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_slot.empty() && g_caption.empty()) return;
  for (auto& [id, c] : g_caption)
    if (c.on && c.rec && (Load32(base, c.rec) >> 16) == id) WriteCaption(base, c, false);
  g_caption.clear();
  g_slot.clear();
  g_real[0] = g_real[1] = 0;
  REXLOG_INFO("[attire] extra attires reset ({})", why);
}
// legacy_roster.h, around its save copy-back: originals out, then ours back.
inline void CaptionsForSave(uint8_t* base, bool after) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto& [id, c] : g_caption)
    if (c.on && c.rec && (Load32(base, c.rec) >> 16) == id) WriteCaption(base, c, after);
}
}  // namespace ufc3_attire

// 3D preview load (preview, side, entry, id, other id, attire, ...): the model id AttireCount needs,
// and what this engine attire stands for from now on. Runs right after sub_829A6EB0 on the same landing.
extern "C" REX_FUNC(sub_8290A770) {
  uint32_t side = ctx.r4.u32 ? 1 : 0, id = ctx.r6.u32, engine = ctx.r8.u32 & 1;
  ufc3_attire::g_side_id[side].store(id);
  if (ufc3_attire::AttireCount(id) > 2) {
    uint32_t real = ufc3_attire::g_real[side].load();
    std::lock_guard<std::mutex> lock(ufc3_attire::g_mutex);
    auto& slot = ufc3_attire::g_slot[id];
    // sub_8290A0A0 skips a load of the same entry and attire: alt -> third is the same engine value, so make it differ.
    if (slot.real[engine] != real) ufc3_attire::Store32(base, ctx.r3.u32 + side * 96 + 104, 0xFFFFFFFFu);
    slot.real[engine] = static_cast<uint8_t>(real);
    ufc3_attire::UpdateCaption(base, id, slot);
  }
  __imp__sub_8290A770(ctx, base);
}

// Attire for (select, side): the stock rule, then the player's pick for that
// fighter on that side.
extern "C" REX_FUNC(sub_829A6EB0) {
  uint32_t select = ctx.r3.u32, side = ctx.r4.u32 ? 1 : 0;
  __imp__sub_829A6EB0(ctx, base);
  if (ufc3_attire::Load32(base, select + 4516) == 0) {  // not exhibition: stock attire 0, no extras
    ufc3_attire::ResetSlots(base, "select screen outside exhibition");
    return;
  }
  if (ufc3_attire::OnlineMode(base, select)) {
    ufc3_attire::ResetSlots(base, "online select screen");
    return;
  }
  uint32_t obj = ufc3_attire::Load32(base, select + ufc3_attire::kSideObj[side]);
  uint32_t id = ufc3_attire::Load32(base, obj + 20);
  uint32_t stock = ctx.r3.u32, real = stock;
  {
    std::lock_guard<std::mutex> lock(ufc3_attire::g_mutex);
    auto it = ufc3_attire::g_pick[side].find(id);
    if (it != ufc3_attire::g_pick[side].end()) real = it->second;
  }
  uint32_t attire = real ? 1 : 0;  // what the game sees
  uint32_t other = side ^ 1, oobj = ufc3_attire::Load32(base, select + ufc3_attire::kSideObj[other]);
  if (real >= 2 || ufc3_attire::g_real[other].load() >= 2)  // only extra attires can collide in a mirror match
    if (ufc3_attire::Load32(base, oobj + 20) == id && ufc3_attire::Load32(base, oobj + 68) == attire &&
        ufc3_attire::g_real[other].load() != real)
      attire ^= 1;
  ufc3_attire::g_real[side].store(real);
  if (attire != stock) {
    ufc3_attire::Store32(base, obj + 68, attire);
    ctx.r3.u64 = attire;
  }
  if (REXCVAR_GET(ufc3_attire_trace))
    REXLOG_INFO("[attire] side {} fighter {} stock {} -> real {} engine {} (mode {}, flag {})", side, id, stock, real, attire,
                ufc3_attire::Load32(base, select + ufc3_attire::kMode), ufc3_attire::Load32(base, select + 4516));
}

// Cursor landed on a fighter (select, entry, side): remember it; a landing that
// isn't the screen's first-frame load of both sides marks the active side.
extern "C" REX_FUNC(sub_829A8EE8) {
  uint32_t side = ctx.r5.u32 ? 1 : 0;
  uint32_t lr = static_cast<uint32_t>(ctx.lr);
  ufc3_attire::g_landing[side] = {ctx.r3.u32, ctx.r4.u32};
  if (lr < 0x829ABD80 || lr >= 0x829ABFD8) ufc3_attire::g_last_side.store(static_cast<int>(side));
  __imp__sub_829A8EE8(ctx, base);
}

// Select screen frame update: apply a queued L3 press once that side's 3D
// preview is idle (load state 0 or 6, as in sub_8290A770).
extern "C" REX_FUNC(sub_829ABD80) {
  uint32_t select = ctx.r3.u32;
  __imp__sub_829ABD80(ctx, base);
  uint64_t now = GetTickCount64();
  ufc3_attire::g_select_tick.store(now, std::memory_order_relaxed);
  ufc3_attire::g_select_online.store(ufc3_attire::OnlineMode(base, select), std::memory_order_relaxed);
  // A 3D preview that finished loading after R3 kept the gloves it was built with: redo the hands then.
  if (uint32_t pv = ufc3_attire::Load32(base, select + ufc3_attire::kPreview)) {
    for (int sd = 0; sd < 2; ++sd) {
      uint32_t st = ufc3_attire::Load32(base, pv + sd * 96 + 52);
      bool idle = st == 0 || st == 6, was_idle = ufc3_attire::g_prev_state[sd] == 0 || ufc3_attire::g_prev_state[sd] == 6;
      if (idle && !was_idle && ufc3_attire::g_on_preview_ready) ufc3_attire::g_on_preview_ready(ctx, base, sd);
      ufc3_attire::g_prev_state[sd] = st;
    }
  }
  int pending = ufc3_attire::g_pending.load();
  if (!pending) return;
  int side = ufc3_attire::g_last_side.load();
  ufc3_attire::Landing landing = ufc3_attire::g_landing[side];
  if (landing.select != select || ufc3_attire::OnlineMode(base, select)) {  // another screen: the presses were for it
    if (REXCVAR_GET(ufc3_attire_trace))
      REXLOG_INFO("[attire] {} presses dropped (side {}, landing {:08X}, select {:08X})", pending, side, landing.select, select);
    ufc3_attire::g_pending.fetch_sub(pending);
    return;
  }
  uint32_t preview = ufc3_attire::Load32(base, select + ufc3_attire::kPreview);
  uint32_t other_state = preview ? ufc3_attire::Load32(base, preview + (side ^ 1) * 96 + 52) : 0;
  uint32_t state = preview ? ufc3_attire::Load32(base, preview + side * 96 + 52) : 0;
  if ((state != 0 && state != 6) || (other_state != 0 && other_state != 6)) return;  // still loading: keep them queued

  uint32_t obj = ufc3_attire::Load32(base, select + ufc3_attire::kSideObj[side]);
  uint32_t id = ufc3_attire::Load32(base, obj + 20);
  uint32_t count = static_cast<uint32_t>(ufc3_attire::AttireCount(ufc3_attire::g_side_id[side].load()));
  uint32_t next = (ufc3_attire::g_real[side].load() + static_cast<uint32_t>(pending)) % count;
  ufc3_attire::g_pending.fetch_sub(pending);
  {
    std::lock_guard<std::mutex> lock(ufc3_attire::g_mutex);
    ufc3_attire::g_pick[side][id] = next;
  }
  PPCContext c = ctx;  // own registers; the callee's frame goes below the caller's r1
  c.r3.u64 = select;
  c.r4.u64 = landing.entry;
  c.r5.u64 = static_cast<uint64_t>(side);
  __imp__sub_829A8EE8(c, base);  // the original, not the hook above: the landing stays as it was
  REXLOG_INFO("[attire] pick side {} fighter {} -> {} ({} presses)", side, id, next, pending);

  // Mirror match: the other side's preview still shows the stock rule's answer
  // from its own landing, while the match keeps the two apart by itself. If that
  // side holds the same fighter without a pick of its own, re-run its landing so
  // the stock rule sees this side's new attire.
  int other = 1 - side;
  ufc3_attire::Landing ol = ufc3_attire::g_landing[other];
  uint32_t oobj = ufc3_attire::Load32(base, select + ufc3_attire::kSideObj[other]);
  bool picked;
  {
    std::lock_guard<std::mutex> lock(ufc3_attire::g_mutex);
    picked = ufc3_attire::g_pick[other].count(id) != 0;
  }
  if (ol.select == select && ufc3_attire::Load32(base, oobj + 20) == id && !picked) {
    c = ctx;
    c.r3.u64 = select;
    c.r4.u64 = ol.entry;
    c.r5.u64 = static_cast<uint64_t>(other);
    __imp__sub_829A8EE8(c, base);
    REXLOG_INFO("[attire] mirror side {} fighter {} -> real {} engine {}", other, id, ufc3_attire::g_real[other].load(), ufc3_attire::Load32(base, oobj + 68));
  }
}

// 2D select picture (r4 side, r5/r6 id, r7 attire): fgt_NNN_0v with v = attire + 2 x glove. Real attire
// n >= 2 reads v = 4(n-1) + 2 x glove: _04/_06, _08/_10... Listed fighters only.
REX_EXTERN(__imp__sub_8290C658);
extern "C" REX_FUNC(sub_8290C658) {
  uint32_t side = ctx.r4.u32 ? 1 : 0;
  if (ufc3_attire::AttireCount(ctx.r6.u32) > 2) {
    uint32_t real = ufc3_attire::g_real[side].load();
    if (REXCVAR_GET(ufc3_attire_trace)) REXLOG_INFO("[attire] picture side {} id {} engine {} real {}", side, ctx.r6.s32, ctx.r7.s32, real);
    ctx.r7.u64 = real < 2 ? real : 4 * (real - 1);
  }
  __imp__sub_8290C658(ctx, base);
}

// Loader: +24 code, +28 id, +32 attire.
namespace ufc3_attire {
constexpr int kMaxFmt = 10;
inline std::atomic<uint32_t> g_fmt{0};
inline bool Listed(uint8_t* mem, uint32_t loader) {
  uint32_t id = Load32(mem, loader + 28);
  return id < 1000 && AttireCount(id) > 2;
}
inline uint32_t LoaderReal(uint8_t* mem, uint32_t loader) {
  return RealAttire(Load32(mem, loader + 28), Load32(mem, loader + 24) % 100);
}
// Guest strings, built once: per attire k >= 2, "%03d_trunks_c{k+1}" then "%03d_trunks_c_b{k+1}".
inline uint32_t Formats(uint8_t* mem) {
  uint32_t f = g_fmt.load();
  if (f) return f;
  uint32_t n = REX_KERNEL_MEMORY()->SystemHeapAlloc(40 * kMaxFmt);
  if (!n) return 0;
  std::memset(mem + n, 0, 40 * kMaxFmt);
  for (int k = 2; k < kMaxFmt; ++k) {
    std::snprintf(reinterpret_cast<char*>(mem + n + 40 * k), 20, "%%03d_trunks_c%d", k + 1);
    std::snprintf(reinterpret_cast<char*>(mem + n + 40 * k + 20), 20, "%%03d_trunks_c_b%d", k + 1);
  }
  uint32_t want = 0;
  if (g_fmt.compare_exchange_strong(want, n)) return n;
  REX_KERNEL_MEMORY()->SystemHeapFree(n);
  return want;
}
}  // namespace ufc3_attire

// 0x82BD0C30 (lwz r9,32(r31)): the real attire picks the path: 0 the main one, anything else the alternate one.
void ufc3_attire_load_begin(PPCRegister& r31) {
  namespace at = ufc3_attire;
  uint8_t* mem = REX_KERNEL_MEMORY()->virtual_membase();
  if (at::Listed(mem, r31.u32)) at::Store32(mem, r31.u32 + 32, at::LoaderReal(mem, r31.u32) ? 1 : 0);
}

// 0x82BD0CA4 (li r6,0, after li r7,2 / addi r8,r1,192): the skip list = every attire but the real one.
// The buffer is per thread: the call that reads it returns before anything else on this thread does.
void ufc3_attire_load_skip(PPCRegister& r7, PPCRegister& r8, PPCRegister& r31) {
  namespace at = ufc3_attire;
  uint8_t* mem = REX_KERNEL_MEMORY()->virtual_membase();
  if (!at::Listed(mem, r31.u32)) return;
  uint32_t id = at::Load32(mem, r31.u32 + 28);
  int count = at::AttireCount(id);
  thread_local uint32_t buf = 0;
  if (!buf) buf = REX_KERNEL_MEMORY()->SystemHeapAlloc(32 * at::kMaxFmt);
  if (!buf) return;
  int real = static_cast<int>(at::LoaderReal(mem, r31.u32));
  uint32_t out = 0;
  for (int k = 0; k < count && k < at::kMaxFmt; ++k) {
    if (k == real) continue;
    char a[17] = {}, b[17] = {};
    if (k == 0) {
      std::snprintf(a, 17, "%03u_trunks_c", id);
      std::snprintf(b, 17, "%03u_trunks_c_b", id);
    } else if (k == 1) {
      std::snprintf(a, 17, "%03u_trunks_c2", id);
      std::snprintf(b, 17, "%03u_trunks_c_b2", id);
    } else {
      std::snprintf(a, 17, "%03u_trunks_c%d", id, k + 1);
      std::snprintf(b, 17, "%03u_trunks_c_b%d", id, k + 1);
    }
    std::memcpy(mem + buf + 16 * out++, a, 16);
    std::memcpy(mem + buf + 16 * out++, b, 16);
  }
  r7.u64 = out;
  r8.u64 = buf;
  if (REXCVAR_GET(ufc3_attire_trace))
    REXLOG_INFO("[attire] load id {} code {} real {} (of {}), {} names skipped", id, at::Load32(mem, r31.u32 + 24), real, count, out);
}

namespace ufc3_attire {
// Trace: the shorts textures the load just put in the fighter's texture list (+2372). List: +4 entries
// {handle, texture}, +8 count; a handle resolves through the table at [0x833C82FC]+8 = {begin, ?, end},
// 8 bytes per handle {handle, texture}; a texture's name is at +16 (sub_82758DE8).
inline std::string LoadedShorts(uint8_t* mem, uint32_t loader) {
  auto ok = [](uint32_t a) { return a >= 0x10000 && a < 0xE0000000u; };
  std::string out;
  uint32_t list = Load32(mem, loader + 2372);
  if (!ok(list)) return "?";
  uint32_t arr = Load32(mem, list + 4), n = Load32(mem, list + 8);
  uint32_t tbl = Load32(mem, 0x833C82FC);
  if (!ok(arr) || n > 4096 || !ok(tbl)) return "?";
  uint32_t begin = Load32(mem, tbl + 8), end = Load32(mem, tbl + 16);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t h = Load32(mem, arr + 8 * i), idx = h & 0xFFFF;
    if (!ok(begin) || begin + idx * 8 + 8 > end || Load32(mem, begin + idx * 8) != h) continue;
    uint32_t tex = Load32(mem, begin + idx * 8 + 4);
    if (!ok(tex)) continue;
    char name[17] = {};
    std::memcpy(name, mem + tex + 16, 16);
    if (std::strstr(name, "_trunks_c")) out += std::string(out.empty() ? "" : " ") + name;
  }
  return out;
}
}  // namespace ufc3_attire

// 0x82BD0CC0 (lwz r9,28(r31), then stw r29,176 / stw r30,184): the names renamed to _c/_c_b.
void ufc3_attire_load_rename(PPCRegister& r29, PPCRegister& r30, PPCRegister& r31) {
  namespace at = ufc3_attire;
  uint8_t* mem = REX_KERNEL_MEMORY()->virtual_membase();
  if (!at::Listed(mem, r31.u32)) return;
  if (REXCVAR_GET(ufc3_attire_trace))
    REXLOG_INFO("[attire] loaded id {} code {}: {}", at::Load32(mem, r31.u32 + 28), at::Load32(mem, r31.u32 + 24),
                at::LoadedShorts(mem, r31.u32));
  uint32_t n = at::LoaderReal(mem, r31.u32);
  if (n < 2 || n >= at::kMaxFmt) return;
  uint32_t f = at::Formats(mem);
  if (!f) return;
  r29.u64 = f + 40 * n;
  r30.u64 = f + 40 * n + 20;
}

// SELECT FIGHTER legend: add L3 and R3. sub_829669B0(bar, nav) rebuilds the UTF-16BE string at
// bar+20132 (128 chars, button tags count) when the page or screen changes, then sets it through
// vtable+124 with index 3. It runs every frame, so "already has our tag" is the normal way out.
// Exe only: the legend table and its labels are data files, and this update has no new mod pack.
REX_EXTERN(__imp__sub_829669B0);
extern "C" REX_FUNC(sub_829669B0) {
  uint32_t bar = ctx.r3.u32, nav = ctx.r4.u32 & 0xFF;
  __imp__sub_829669B0(ctx, base);
  if (!REXCVAR_GET(ufc3_select_hints) || nav != 17 || !ufc3_pack::Active()) return;  // 17 = SELECT FIGHTER; 19 = venue screen, where the select frame still ticks
  if (GetTickCount64() - ufc3_attire::g_select_tick.load(std::memory_order_relaxed) > 500) return;
  if (ufc3_attire::g_select_online.load(std::memory_order_relaxed)) return;
  constexpr uint32_t kText = 20132, kMax = 128;
  const bool picker = REXCVAR_GET(ufc3_attire_picker);
  const char* kAdd = picker ? "<&L3_LS>ATTIRE  <&R3_RS>BARE KNUCKLE  " : "<&R3_RS>BARE KNUCKLE  ";
  const uint32_t add_len = static_cast<uint32_t>(std::strlen(kAdd));
  uint32_t str = bar + kText, len = 0;
  auto at = [&](uint32_t i) { return static_cast<uint16_t>((base[str + i * 2] << 8) | base[str + i * 2 + 1]); };
  while (len < kMax && at(len)) ++len;
  if (len == kMax) return;
  for (uint32_t i = 0; i + 8 <= len; ++i) {  // our hint is there already
    const char* kTag = picker ? "<&L3_LS>" : "<&R3_RS>";
    uint32_t j = 0;
    while (j < 8 && at(i + j) == static_cast<uint8_t>(kTag[j])) ++j;
    if (j == 8) return;
  }
  bool fits = len + add_len + 1 <= kMax;
  if (REXCVAR_GET(ufc3_attire_trace))
    REXLOG_INFO("[attire] legend nav {} page {} len {} {}", nav, ufc3_attire::Load32(base, bar + 20128), len, fits ? "+hints" : "no room");
  if (!fits) return;
  for (uint32_t i = 0; i <= add_len; ++i) {
    base[str + (len + i) * 2] = 0;
    base[str + (len + i) * 2 + 1] = static_cast<uint8_t>(kAdd[i]);
  }
  PPCContext saved = ctx;
  ctx.r3.u64 = bar;
  ctx.r4.u64 = 3;
  ctx.r5.u64 = str;
  uint32_t vtable = ufc3_attire::Load32(base, bar);
  ctx.ctr.u64 = ufc3_attire::Load32(base, vtable + 124);
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
  ctx = saved;
}
