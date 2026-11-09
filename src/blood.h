// ufc3 - more blood, visual only (FINDINGS-damage.md sections 1-2, item DMG2).
//
// sub_82C722C0 turns each face zone's peak bleed points into a level 0..1 and
// hands (level - 0.1) to the blood component (fighter +0xEC8, vtable 0x8214F478)
// through SetLevel (slot 1, sub_82D50230). Face regions have no delay: every new
// level value pastes the region's "ba0" brush into the liquid map (sub_8310C8E0,
// skipped while that brush still runs), so the level only decides WHEN a zone
// shows blood (> 0.1) and every change re-starts the flow. Chest/back regions
// (11-17) have a delay of 30: a SetLevel there starts a timer and pastes the
// "bb0" (level < 0.5) or "bb1" brush when it runs out.
//
// ufc3_blood = normal | more | max, applied at 0x82C72670 (f1 = level, after the
// step event, before the -0.1; cut counters, bleed points and the step event at
// dmg+0x204 ran already, so gameplay is untouched):
//   more: level * 1.6, blood runs down onto the next region at 0.5x, each
//         bleeding region re-flows every 4 s, chest at 0.4x of the worst face zone
//   max:  any bleeding zone at least 0.95, run-down at 0.8x, re-flow every 1.5 s,
//         chest at 0.25 like more (bb0 red; 0.5+ pasted the pale bb1 stage) but
//         its delay cut from 30 to 10, so it re-flows three times as often
// Debug: ufc3_blood_force = zones "4,1" (zone map 0x8213ABC8; 1-7, 0 = forehead
// uses a different counter and is ignored) keeps those zones' peak bleed points
// at ufc3_blood_force_level for the whole fight, so a scripted fight bleeds.
//
// Bloody knuckles (DMG4, FINDINGS-damage.md section 3), bare hands only, any
// ufc3_blood mode. The liquid builder sub_8310E2F8 turns each blood.map rect
// {->name, u0 v0 u1 v1} (map header at liquid +0x68: {1, count, ->rects, ->x})
// into entries (liquid +0x20, count +4, stride 0x90: +4 ->rect, +0x18 name key
// of 32 bytes), one per flow frame of "<name>.yda" in liquid.pac (no .yda = no
// entry, so only ba0 brushes build). Before it runs, the map is swapped for a
// copy with two knuckle rects that borrow ufc3_knuckle_brush; after it, their
// entries are renamed 00_lhand_bb0 / 00_rhand_bb0, so only a paste by that name
// (sub_8310C8E0) reaches them. One rect pair fits disc and 2009-part bodies
// (their hand UVs differ by < 0.003): the back of the hand from the knuckles
// down (each hand island is palm, then back with the nails; the brush flows
// towards the wrist). While a fighter's face bleeds, the other
// fighter's knuckles get a paste when the worst face level steps up and again
// every 6 s (more 4 s, max 2 s); both hands (no hit-limb info).
// The hands' own shader has no liquid map (DMG4b): ufc3_knuckle_zone moves the
// hand meshes to the zone whose shader has one (ZoneHands below).

#pragma once

#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "bare_knuckle.h"
#include "mod_pack.h"

REXCVAR_DEFINE_STRING(ufc3_blood, "max", "ufc3",
                      "Bare knuckle blood amount, visual only: normal, more or max (normal = stock everywhere)");
REXCVAR_DEFINE_STRING(ufc3_blood_gloves, "light", "ufc3",
                      "Gloved blood: light (slightly above stock), stock, or full (same as ufc3_blood)");
REXCVAR_DEFINE_DOUBLE(ufc3_blood_red, 0.55, "ufc3", "Blood tint red on fighter skin, 0..1, set at model load (stock 0.55, UFC 2009 ports 0.66)");
REXCVAR_DEFINE_DOUBLE(ufc3_blood_max_gain, 2.0, "ufc3", "Heavy blood: how much a cut's bleed level is multiplied on screen");
REXCVAR_DEFINE_DOUBLE(ufc3_blood_max_floor, 0.25, "ufc3", "Heavy blood: lowest on-screen level of an open cut, 0..1");
REXCVAR_DEFINE_DOUBLE(ufc3_blood_max_rise, 0.0, "ufc3", "Heavy blood: most a zone's bleed level can rise per minute (0 = no limit)");
REXCVAR_DEFINE_BOOL(ufc3_blood_trace, false, "ufc3", "Log face blood levels ([blood] lines; debug)");
REXCVAR_DEFINE_STRING(ufc3_blood_force, "", "ufc3",
                      "Debug: face zones that always bleed, e.g. 4,1 (4 = nose, 1 = left eye); empty = off");
REXCVAR_DEFINE_DOUBLE(ufc3_blood_force_level, 0.3, "ufc3", "Debug: blood level 0..1 that ufc3_blood_force keeps");
REXCVAR_DEFINE_BOOL(ufc3_blood_stamps, true, "ufc3",
                    "Thick and knuckle blood from the wide stamp brushes (off = stretched streak brushes, the vein look)");
REXCVAR_DEFINE_BOOL(ufc3_blood_soft, true, "ufc3",
                    "Thick blood layers from the generated soft brushes in the mod's fighter_cmn.pac (off or no such file = stamps)");
REXCVAR_DEFINE_BOOL(ufc3_blood_replay_keep, true, "ufc3",
                    "Replays keep the blood already on the skin; off = the game's restore, which clears it and redraws slowly");
REXCVAR_DEFINE_BOOL(ufc3_blood_chest_flows, true, "ufc3",
                    "Bare chest uses the stock flows (run down the skin, but start from a hard round blob); off = soft brushes only, in stages");
REXCVAR_DEFINE_BOOL(ufc3_blood_stain, false, "ufc3",
                    "Leave a blood stain after the corner wash between rounds (off = full wash like stock; cuts stay either way)");
REXCVAR_DEFINE_BOOL(ufc3_knuckle_blood, true, "ufc3",
                    "Bare hands: the opponent's face blood shows on the striker's knuckles");
REXCVAR_DEFINE_STRING(ufc3_knuckle_brush, "00_nose_ba0", "ufc3",
                      "Debug: liquid.pac brush the knuckle blood borrows (a ba0 one, it needs a .yda)");
REXCVAR_DEFINE_STRING(ufc3_knuckle_rect, "", "ufc3",
                      "Debug: u0,v0,u1,v1 skin-UV rect used for both knuckle rects instead of the hands (empty = hands)");
REXCVAR_DEFINE_STRING(ufc3_knuckle_zone, "all", "ufc3",
                      "Debug: draw hand meshes with the liquid-map skin shader (zone 2): off, all or one fighter id, e.g. 091");
REXCVAR_DECLARE(bool, ufc3_knuckle_trace);  // src/reblue/gpu/hooks/state.cpp

namespace ufc3_blood {

enum Mode : int { kNormal = 0, kMore = 1, kMax = 2 };

// The ufc3_blood setting. The heavy look belongs to bare knuckle (user 2026-11-05): gloved
// fights stay close to the original game, see GetMode and GlovedLift.
inline int SettingMode() {
  static const int m = [] {
    if (!ufc3_pack::Active()) return kNormal;  // stock mode: Blood and Guts off
    const std::string& v = REXCVAR_GET(ufc3_blood);
    return v == "max" ? kMax : v == "more" ? kMore : kNormal;
  }();
  return m;
}
enum Gloved : int { kGlovedStock = 0, kGlovedLight = 1, kGlovedFull = 2 };
inline int GlovedSetting() {
  static const int g = [] {
    if (!ufc3_pack::Active()) return kGlovedStock;
    const std::string& v = REXCVAR_GET(ufc3_blood_gloves);
    return v == "full" ? kGlovedFull : v == "stock" ? kGlovedStock : kGlovedLight;
  }();
  return g;
}
inline int GetMode() {
  if (ufc3_bk::Hands() == ufc3_bk::kGloves && GlovedSetting() != kGlovedFull) return kNormal;
  return SettingMode();
}
// Gloved "light": every zone level x this, nothing else differs from stock.
inline float GlovedLift() {
  bool light = ufc3_bk::Hands() == ufc3_bk::kGloves && GlovedSetting() == kGlovedLight && SettingMode() != kNormal;
  return light ? 1.5f : 1.0f;  // 1.25 could not be told from stock (dmg4z8-gloved-real vs -stock)
}

constexpr uint32_t kBloodVtable = 0x8214F478;
constexpr uint32_t kZoneMap = 0x8213ABC8;    // 8 x {region, alt region, side flag, special}
constexpr uint32_t kDivisor = 0x8333B074;    // [0x8333A0B0+0xFC4]: peak bleed points per 1.0 level
constexpr int kRegions = 19;
constexpr int kChest[] = {11, 12, 13};       // cchest, lchest, rchest

// Blood runs down: a bleeding region also raises the region below it.
struct Spread {
  int from, to;
};
constexpr Spread kSpread[] = {{0, 1},  {18, 2}, {1, 3}, {2, 4}, {3, 9},   {4, 10},
                              {6, 5},  {5, 7},  {5, 8}};

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}
inline void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = _byteswap_ulong(v);
  std::memcpy(base + addr, &v, 4);
}
inline float LoadF(uint8_t* base, uint32_t addr) {
  uint32_t u = Load32(base, addr);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
inline void StoreF(uint8_t* base, uint32_t addr, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  Store32(base, addr, u);
}

// One per damage object (one per fighter); the game thread is the only user.
struct State {
  uint32_t dmg = 0;
  uint32_t blood = 0;      // the fighter's blood component (SetLevel's this)
  float face_raw = 0;      // last frame's worst unscaled face level (chest gate)
  uint32_t frame = 0;
  float own[kRegions] = {};   // this frame's scaled level per region (before the -0.1)
  float prev[kRegions] = {};  // last frame's, feeds the run-down
  float traced[kRegions] = {};
  float raw[kRegions] = {};       // this frame's level before the ufc3_blood scaling
  float ramp[kRegions] = {};      // level after the rise limit (ufc3_blood_max_rise)
  float prev_raw[kRegions] = {};  // last frame's (regions run in order: a later source isn't set yet)
  float chest = 0;
  int knuckle_step = 0;  // worst face level step the opponent's knuckles were last pasted for
  uint32_t knuckle_frame = 0;
  int knuckle_layers = 0;  // layers pasted on the opponent so far (dmg4s)
  int face_added = 0;      // extra face regions pasted so far (dmg4t)
  uint32_t face_stamped = 0;  // regions whose bb0 stamp went on (bit per region)
  uint32_t face_tick = 0;     // frame of the last face build-up step
  int face_side = -1;         // 0 = left regions first, 1 = right
  uint32_t chest_frame = 0;  // last own chest paste (dmg4q)
  int thick_added = 0;     // thick face layers pasted so far (dmg4z)
  int chest_layers = 0;    // chest layers pasted so far: 0 none, 1 ba0, then thick (dmg4z)
  uint32_t residue = 0;    // regions that show a stain after a corner wash (bit per region, dmg4z)
  uint32_t bled = 0;       // face regions that bled this round (dmg4z3)
  float raw_held = 0;      // last non-zero worst raw level (dmg4z2)
  uint32_t raw_zero = 0;   // frames the raw level has read zero in a row
};

constexpr float kMaxChestDelay = 10.0f;  // max: chest timer cap (region table delay 30)

// Knuckle rects {u0, v0, u1, v1} in the skin UV (fighter's own left/right hand).
struct Knuckle {
  const char* name;
  float rect[4];
  const char* brush;  // nullptr = ufc3_knuckle_brush
};
// User 2026-11-04 (watched dmg4f3-allisle): the whole hand island as pasted there,
// on BOTH hands, and more blood -> each hand island gets the cvar brush plus the
// thick two-stream rmouth brush on top.
// dmg4j (user drove): the whole-island layers put blood on the body and drew
// the streak, the small splatter spots looked right -> splatter spots only, two
// brushes each (more blood). Right hand = the left spots mirrored in u.
constexpr Knuckle kKnuckles[] = {{"00_lhand_bb0", {0.876f, 0.685f, 0.958f, 0.755f}, "00_lcheek_ba0"},
                                 {"00_rhand_bb0", {0.042f, 0.685f, 0.124f, 0.755f}, "00_rcheek_ba0"},
                                 {"00_lhand_bb1", {0.876f, 0.685f, 0.958f, 0.755f}, nullptr},
                                 {"00_rhand_bb1", {0.042f, 0.685f, 0.124f, 0.755f}, nullptr},
                                 {"00_lhand_bb2", {0.820f, 0.700f, 0.990f, 0.800f}, "00_leye_b_ba0"},
                                 {"00_rhand_bb2", {0.010f, 0.700f, 0.180f, 0.800f}, "00_reye_b_ba0"},
                                 {"00_lhand_bb3", {0.820f, 0.700f, 0.990f, 0.800f}, "00_rcheek_ba0"},
                                 {"00_rhand_bb3", {0.010f, 0.700f, 0.180f, 0.800f}, "00_lcheek_ba0"},
                                 {"00_lhand_bb4", {0.900f, 0.630f, 0.990f, 0.720f}, "00_rcheek_ba0"},
                                 {"00_rhand_bb4", {0.010f, 0.630f, 0.100f, 0.720f}, "00_lcheek_ba0"}};
constexpr int kNumKnuckles = 10;
// Hand-mesh detection (ufc3_knuckle_zone): the knuckle rings only, so no arm or
// foot mesh counts as a hand.
constexpr Knuckle kHandDetect[] = {{"lhand", {0.876f, 0.685f, 0.958f, 0.755f}, nullptr},
                                   {"rhand", {0.042f, 0.685f, 0.124f, 0.755f}, nullptr}};
constexpr uint32_t kDisplayFlags = 0x83541FE0 + 0x32980;  // & 0x30 = blood shown (sub_82D50840 checks it)

// dmg4z (user: thick areas, a mask of blood by the end): every brush is one image that
// plays once per round and repainting it adds nothing (FINDINGS-liquid.md), so a face
// tops out at thin single streaks. Thick layers: each flow brush again on wider, then
// wider and longer rects, so its streaks draw wider and merge over the first pass.
constexpr const char* kThickSources[] = {"00_fore_l", "00_leye_t", "00_reye_t", "00_leye_b", "00_reye_b", "00_nose",   "00_bridge",
                                         "00_lmouth", "00_rmouth", "00_lcheek", "00_rcheek", "00_cchest", "00_lchest", "00_rchest",
                                         "00_fore_r"};
constexpr int kThickSrc = 15;  // regions 0-13 in table order, then region 18
inline int ThickSource(int region) { return region == 18 ? 14 : region; }
constexpr int kThickLayers = 3;
constexpr int kNumThick = kThickSrc * kThickLayers;
constexpr int kThickChestSrc = 11;
inline std::atomic<int64_t> g_paint_pause_until{0};  // ms, GetTickCount64: no new blood during a doctor visit (cuts.h)
inline bool PaintPaused() { return static_cast<int64_t>(GetTickCount64()) < g_paint_pause_until.load(); }
inline std::atomic<uint32_t> g_thick_names{0};  // guest copies, 16 bytes each: source * layers + layer
inline void ThickRect(const float in[4], int layer, float out[4]) {
  static constexpr float kWide[kThickLayers] = {1.7f, 1.5f, 2.1f}, kLong[kThickLayers] = {1.0f, 1.15f, 1.3f};
  float w = in[2] - in[0], cx = (in[0] + in[2]) * 0.5f;
  float wide = kWide[layer];
  if (layer == 1) cx += (cx >= 0.5f ? 0.3f : -0.3f) * w;  // pushed outwards from the middle of the body
  out[0] = std::max(0.0f, cx - w * wide * 0.5f);
  out[2] = std::min(1.0f, cx + w * wide * 0.5f);
  out[1] = in[1];
  out[3] = std::min(1.0f, in[1] + (in[3] - in[1]) * kLong[layer]);
}

// The model loaded last: its liquid is built right after it (seen 1:1 in dmg4z3-heavy test.log).
// is_2009 = its skin carries UFC 2009's blood colour.
inline std::string g_last_id;
inline bool g_last_2009 = false;
inline std::unordered_set<std::string> g_ids_2009;

// Chest blood rects the port tool left on the donor's layout (head_bone took the chest fat bones
// for the face rig; fixed in port_legacy.py, FINDINGS-blood-imbalance.md). Same values as a fresh
// build, so this table does nothing once these four pacs are rebuilt.
struct RectFix {
  const char* id;
  const char* name;
  float rect[4];
};
constexpr RectFix kRectFixes[] = {
    {"046", "00_lchest_bb0", {0.5145f, 0.3849f, 0.5950f, 0.4563f}}, {"046", "00_lchest_bb1", {0.5145f, 0.3849f, 0.5950f, 0.4563f}},
    {"046", "00_rchest_bb0", {0.4145f, 0.3839f, 0.4950f, 0.4584f}}, {"046", "00_rchest_bb1", {0.4145f, 0.3839f, 0.4950f, 0.4584f}},
    {"046", "00_lchest_ba0", {0.5361f, 0.3952f, 0.5972f, 0.5013f}}, {"046", "00_rchest_ba0", {0.4207f, 0.3967f, 0.4778f, 0.4924f}},
    {"061", "00_lchest_bb0", {0.5059f, 0.3824f, 0.6137f, 0.4809f}}, {"061", "00_lchest_bb1", {0.5059f, 0.3824f, 0.6137f, 0.4809f}},
    {"061", "00_rchest_bb0", {0.3942f, 0.3745f, 0.5000f, 0.4765f}}, {"061", "00_rchest_bb1", {0.3942f, 0.3745f, 0.5000f, 0.4765f}},
    {"061", "00_lchest_ba0", {0.5368f, 0.3963f, 0.6203f, 0.5402f}}, {"061", "00_rchest_ba0", {0.3990f, 0.3956f, 0.4798f, 0.5214f}},
    {"161", "00_lchest_bb0", {0.5020f, 0.3749f, 0.5695f, 0.4429f}}, {"161", "00_lchest_bb1", {0.5020f, 0.3749f, 0.5695f, 0.4429f}},
    {"161", "00_lchest_ba0", {0.5185f, 0.3851f, 0.5988f, 0.4922f}}, {"161", "00_rchest_ba0", {0.4367f, 0.3793f, 0.4899f, 0.4714f}},
    {"174", "00_lchest_bb0", {0.5027f, 0.3795f, 0.5837f, 0.4563f}}, {"174", "00_lchest_bb1", {0.5027f, 0.3795f, 0.5837f, 0.4563f}},
    {"174", "00_rchest_bb0", {0.4207f, 0.3655f, 0.5032f, 0.4522f}}, {"174", "00_rchest_bb1", {0.4207f, 0.3655f, 0.5032f, 0.4522f}},
    {"174", "00_lchest_ba0", {0.5242f, 0.3910f, 0.5893f, 0.4993f}}, {"174", "00_rchest_ba0", {0.4171f, 0.3850f, 0.4867f, 0.4785f}},
};

struct MapCopy {
  uint32_t buf = 0, size = 0, count = 0;  // guest copy, its size, the original rect count
  bool is_2009 = false;                    // body on the UFC 2009 skin atlas
  bool alt_set = false;                    // uses kSoftSlotsAlt
};
inline std::mutex g_map_mutex;
inline std::unordered_map<uint32_t, MapCopy> g_maps;  // liquid object -> its knuckle map copy
struct SavedState {  // a liquid state too big for the game's 64-byte buffers
  uint8_t head[64];
  std::vector<uint8_t> full;
};
inline std::unordered_map<uint64_t, std::vector<SavedState>> g_saved;
inline std::mutex g_saved_mutex;
inline std::atomic<uint32_t> g_names{0};              // guest copies of the knuckle names, 16 bytes each

inline bool KnucklesOn() { return ufc3_pack::Active() && REXCVAR_GET(ufc3_knuckle_blood) && ufc3_bk::Hands() != ufc3_bk::kGloves; }

inline bool NameIs(uint8_t* base, uint32_t ptr, const std::string& want) {
  if (!ptr) return false;
  const char* s = reinterpret_cast<const char*>(base + ptr);
  return std::strncmp(s, want.c_str(), want.size() + 1) == 0;
}

// tools/gen_soft_brushes.py writes soft shapes into the 8 back and shoulder stamps of an overlaid fighter_cmn.pac: stock never
// pastes those, and adding entries to that pac crashes the game. One slot per entry of kThickSources (same order); the tool's
// SLOTS holds the shapes. Without the file the thick layers keep each source's own stamp.
constexpr const char* kSoftSlots[kThickSrc] = {"00_rback_bb1",  "00_lshoul_bb0", "00_lback_bb1",  "00_lshoul_bb0", "00_lback_bb1",
                                               "00_lback_bb0",  "00_lback_bb0",  "00_lback_bb1",  "00_rback_bb0",  "00_lback_bb1",
                                               "00_rback_bb0",  "00_rshoul_bb0", "00_rshoul_bb1", "00_lback_bb1",  "00_rback_bb1"};
// The second fighter built gets another set, so the two don't wear the same pattern.
constexpr const char* kSoftSlotsAlt[kThickSrc] = {"00_lshoul_bb0", "00_rback_bb1",  "00_lback_bb1",  "00_rback_bb1",  "00_lback_bb1",
                                                  "00_rback_bb0",  "00_lback_bb0",  "00_lback_bb0",  "00_lback_bb1",  "00_rback_bb0",
                                                  "00_lback_bb0",  "00_rshoul_bb1", "00_rshoul_bb0", "00_rback_bb0",  "00_lshoul_bb0"};
// The post-fight wipe (user: corner clean stays, dried look on hands and face after the fight only): one low soft brush over the
// face and both hands, pasted once at the fight end. The paste blend fades what's under it and tints the skin.
constexpr const char* kWipeSlot = "00_lshoul_bb1";
constexpr int kNumWipe = 3;
constexpr float kWipeRects[kNumWipe][4] = {{0.38f, 0.015f, 0.62f, 0.27f}, {0.82f, 0.63f, 0.99f, 0.80f}, {0.01f, 0.63f, 0.18f, 0.80f}};
constexpr const char* kWipeNames[kNumWipe] = {"00_wipe_face", "00_wipe_lhand", "00_wipe_rhand"};
inline std::atomic<uint32_t> g_wipe_names{0};
inline bool SoftBrushesInstalled() {
  static const bool found = [] {
    std::string mods = rex::cvar::GetFlagByName("mods_root");
    if (mods.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(rex::to_path(mods) / "DAT" / "chara" / "fighter_cmn.pac", ec);
  }();
  return found;
}
// Copy of the blood map with the knuckle rects appended (they take the borrowed
// brush's name until the builder ran), or 0 to leave the map alone.
inline uint32_t ExtendMap(uint8_t* base, uint32_t liquid, uint32_t map) {
  std::lock_guard<std::mutex> lock(g_map_mutex);
  MapCopy& mc = g_maps[liquid];
  if (mc.buf && map == mc.buf) return map;  // a rebuild on the map we installed
  uint32_t count = Load32(base, map + 4), rects = Load32(base, map + 8);
  if (!count || count > 512 || !rects) return 0;
  auto find = [&](const std::string& name) {
    for (uint32_t i = 0; i < count; ++i)
      if (NameIs(base, Load32(base, rects + i * 20), name)) return Load32(base, rects + i * 20);
    return 0u;
  };
  if (REXCVAR_GET(ufc3_blood_trace)) {
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t r = rects + i * 20, nm = Load32(base, r);
      REXLOG_INFO("[blood] map {:08X} rect {:2}: {:<16} {:.3f},{:.3f} - {:.3f},{:.3f}", liquid, i,
                  nm ? reinterpret_cast<const char*>(base + nm) : "-", LoadF(base, r + 4), LoadF(base, r + 8),
                  LoadF(base, r + 12), LoadF(base, r + 16));
    }
  }
  const std::string& main_brush = REXCVAR_GET(ufc3_knuckle_brush);
  // Blood look: flows (ba0) are thin branching streaks, stretched they cross into a mesh of veins, and a flow stopped
  // at its hold ends in a straight edge on the fingers. Stamps (bb0) are wide, blotchy and soft (FINDINGS-blood-look.md).
  auto stamped = [&](std::string name) {
    if (REXCVAR_GET(ufc3_blood_stamps) && name.size() > 4 && name.compare(name.size() - 4, 4, "_ba0") == 0) {
      std::string bb = name.substr(0, name.size() - 4) + "_bb0";
      if (find(bb)) return bb;
    }
    return name;
  };
  uint32_t main_src = find(stamped(main_brush));
  if (!main_src) {
    REXLOG_INFO("[blood] liquid {:08X}: no {} rect in its blood map, no knuckle blood", liquid, main_brush);
    return 0;
  }
  uint32_t srcs[kNumKnuckles] = {};
  for (int k = 0; k < kNumKnuckles; ++k) {
    srcs[k] = kKnuckles[k].brush ? find(stamped(kKnuckles[k].brush)) : main_src;
    if (!srcs[k]) {  // that brush is missing: the main one
      REXLOG_INFO("[blood] liquid {:08X}: no {} brush, layer {} uses {}", liquid, kKnuckles[k].brush, k, main_brush);
      srcs[k] = main_src;
    }
  }
  if (!g_names.load()) {
    uint32_t n = REX_KERNEL_MEMORY()->SystemHeapAlloc(16 * kNumKnuckles);
    if (!n) return 0;
    std::memset(base + n, 0, 16 * kNumKnuckles);
    for (int k = 0; k < kNumKnuckles; ++k) std::memcpy(base + n + 16 * k, kKnuckles[k].name, std::strlen(kKnuckles[k].name));
    g_names.store(n);
  }
  if (!g_thick_names.load()) {
    uint32_t n = REX_KERNEL_MEMORY()->SystemHeapAlloc(16 * kNumThick);
    if (!n) return 0;
    std::memset(base + n, 0, 16 * kNumThick);
    for (int t = 0; t < kNumThick; ++t) {
      std::string nm = std::string(kThickSources[t / kThickLayers]) + "_tk" + std::to_string(t % kThickLayers);
      std::memcpy(base + n + 16 * t, nm.c_str(), nm.size());
    }
    g_thick_names.store(n);
  }
  if (!g_wipe_names.load()) {
    uint32_t n = REX_KERNEL_MEMORY()->SystemHeapAlloc(16 * kNumWipe);
    if (!n) return 0;
    std::memset(base + n, 0, 16 * kNumWipe);
    for (int w = 0; w < kNumWipe; ++w) std::memcpy(base + n + 16 * w, kWipeNames[w], std::strlen(kWipeNames[w]));
    g_wipe_names.store(n);
  }
  uint32_t size = 16 + (count + kNumKnuckles + kNumThick + kNumWipe) * 20;
  if (mc.size < size) {
    if (mc.buf) REX_KERNEL_MEMORY()->SystemHeapFree(mc.buf);
    mc.buf = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
    mc.size = mc.buf ? size : 0;
    if (!mc.buf) return 0;
  }
  uint32_t c = mc.buf;
  Store32(base, c, Load32(base, map));
  Store32(base, c + 4, count + kNumKnuckles + kNumThick + kNumWipe);
  Store32(base, c + 8, c + 16);
  Store32(base, c + 12, Load32(base, map + 12));
  std::memcpy(base + c + 16, base + rects, count * 20);
  for (int k = 0; k < kNumKnuckles; ++k) {
    uint32_t r = c + 16 + (count + k) * 20;
    Store32(base, r, srcs[k]);
    float rect[4];
    std::memcpy(rect, kKnuckles[k].rect, sizeof(rect));
    const std::string& dbg = REXCVAR_GET(ufc3_knuckle_rect);
    const char* p = dbg.c_str();
    for (int j = 0; j < 4 && *p; ++j) {
      char* end;
      rect[j] = std::strtof(p, &end);
      p = *end == ',' ? end + 1 : end;
    }
    for (int j = 0; j < 4; ++j) StoreF(base, r + 4 + 4 * j, rect[j]);
  }
  int fixed = 0;
  std::string model_id = g_last_id;
  // 061 kept its donor's texture names, so its models say 027 (no fighter 027 exists on the disc or in the mod).
  mc.is_2009 = g_last_2009;
  const std::string rect_key = model_id == "027" ? "061" : model_id;
  for (const RectFix& fix : kRectFixes) {
    if (rect_key != fix.id) continue;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t r = c + 16 + i * 20;
      if (!NameIs(base, Load32(base, r), fix.name)) continue;
      for (int j = 0; j < 4; ++j) StoreF(base, r + 4 + 4 * j, fix.rect[j]);
      ++fixed;
    }
  }
  REXLOG_INFO("[blood] liquid {:08X}: model '{}', 2009 atlas {}, {} chest rects corrected", liquid, model_id, mc.is_2009, fixed);
  static std::atomic<int> builds{0};
  if (!mc.count) mc.alt_set = (builds.fetch_add(1) & 1) != 0;
  const bool alt_set = mc.alt_set;
  // Thick layers borrow the brush's rect name so they build its .yda; renamed after the build.
  for (int t = 0; t < kNumThick; ++t) {
    uint32_t r = c + 16 + (count + kNumKnuckles + t) * 20;
    std::string src = stamped(std::string(kThickSources[t / kThickLayers]) + "_ba0");
    uint32_t src_rect = 0;
    for (uint32_t i = 0; i < count && !src_rect; ++i)
      if (NameIs(base, Load32(base, c + 16 + i * 20), src)) src_rect = c + 16 + i * 20;
    if (!src_rect) {  // no such brush on this body: an empty rect that never gets a paste
      std::memcpy(base + r, base + rects, 20);
      for (int j = 0; j < 4; ++j) StoreF(base, r + 4 + 4 * j, 0.0f);
      continue;
    }
    float in[4], out[4];
    for (int j = 0; j < 4; ++j) in[j] = LoadF(base, src_rect + 4 + 4 * j);
    ThickRect(in, t % kThickLayers, out);
    uint32_t soft = REXCVAR_GET(ufc3_blood_soft) && REXCVAR_GET(ufc3_blood_stamps) && SoftBrushesInstalled()
                        ? find((alt_set ? kSoftSlotsAlt : kSoftSlots)[t / kThickLayers])
                        : 0;
    Store32(base, r, soft ? soft : Load32(base, src_rect));
    for (int j = 0; j < 4; ++j) StoreF(base, r + 4 + 4 * j, out[j]);
  }
  uint32_t wipe_src = REXCVAR_GET(ufc3_blood_soft) && SoftBrushesInstalled() ? find(kWipeSlot) : 0;
  for (int w = 0; w < kNumWipe; ++w) {
    uint32_t r = c + 16 + (count + kNumKnuckles + kNumThick + w) * 20;
    if (wipe_src) {
      Store32(base, r, wipe_src);
      for (int j = 0; j < 4; ++j) StoreF(base, r + 4 + 4 * j, kWipeRects[w][j]);
    } else {  // an empty rect that never gets a paste
      std::memcpy(base + r, base + rects, 20);
      for (int j = 0; j < 4; ++j) StoreF(base, r + 4 + 4 * j, 0.0f);
    }
  }
  mc.count = count;
  return c;
}

// After the builder: entries made from the knuckle rects get the knuckle names.
inline void RenameKnuckles(uint8_t* base, uint32_t liquid, uint32_t copy) {
  uint32_t count;
  {
    std::lock_guard<std::mutex> lock(g_map_mutex);
    count = g_maps[liquid].count;
  }
  uint32_t n = Load32(base, liquid + 4), arr = Load32(base, liquid + 0x20);
  int renamed[kNumKnuckles] = {};
  int thick = 0;
  uint32_t thick_names = g_thick_names.load();
  for (uint32_t i = 0; arr && i < n && i < 4096; ++i) {
    uint32_t e = arr + i * 0x90;
    uint32_t rect = Load32(base, e + 4);
    for (int k = 0; k < kNumKnuckles; ++k) {
      if (rect != copy + 16 + (count + k) * 20) continue;
      std::memset(base + e + 0x18, 0, 32);
      std::memcpy(base + e + 0x18, kKnuckles[k].name, std::strlen(kKnuckles[k].name));
      ++renamed[k];
    }
    uint32_t first_wipe = copy + 16 + (count + kNumKnuckles + kNumThick) * 20, wipe_names = g_wipe_names.load();
    if (wipe_names && rect >= first_wipe && rect < first_wipe + kNumWipe * 20) {
      std::memset(base + e + 0x18, 0, 32);
      std::memcpy(base + e + 0x18, base + wipe_names + 16 * ((rect - first_wipe) / 20), 16);
    }
    uint32_t first_thick = copy + 16 + (count + kNumKnuckles) * 20;
    if (thick_names && rect >= first_thick && rect < first_thick + kNumThick * 20) {
      uint32_t t = (rect - first_thick) / 20;
      std::memset(base + e + 0x18, 0, 32);
      std::memcpy(base + e + 0x18, base + thick_names + 16 * t, 16);
      ++thick;
    }
  }
  std::string got;
  for (int k = 0; k < kNumKnuckles; ++k) got += " " + std::to_string(renamed[k]);
  REXLOG_INFO("[blood] liquid {:08X}: {} map rects + {} knuckle rects ({} + extra layers) -> {} entries, knuckle entries{}",
              liquid, count, kNumKnuckles, REXCVAR_GET(ufc3_knuckle_brush), n, got + " thick " + std::to_string(thick));
}

// Fighters whose face update ran lately: the other one is the opponent.
struct Seen {
  uint32_t fighter = 0;
  uint64_t tick = 0;
  bool liquid = false;  // has a liquid map (a real fighter body, not a third damage object)
};
inline Seen g_seen[4];

inline uint32_t Opponent(uint32_t fighter, bool has_liquid) {
  uint64_t now = GetTickCount64();
  int slot = -1, oldest = 0;
  for (int i = 0; i < 4; ++i) {
    if (g_seen[i].fighter == fighter) slot = i;
    if (g_seen[i].tick < g_seen[oldest].tick) oldest = i;
  }
  g_seen[slot < 0 ? oldest : slot] = {fighter, now, has_liquid};
  uint32_t opp = 0;
  uint64_t best = 0;
  for (const auto& s : g_seen)
    if (s.fighter && s.liquid && s.fighter != fighter && now - s.tick < 1000 && s.tick > best) opp = s.fighter, best = s.tick;
  return opp;
}
inline State g_states[4];
inline State* g_cur = nullptr;  // set around the original sub_82C722C0

inline State& StateFor(uint32_t dmg) {
  for (auto& s : g_states)
    if (s.dmg == dmg) return s;
  static int next = 0;
  State& s = g_states[next++ & 3];
  s = State{};
  s.dmg = dmg;
  return s;
}

inline const std::vector<int>& ForcedZones() {
  static const std::vector<int> z = [] {
    std::vector<int> out;
    const std::string& v = REXCVAR_GET(ufc3_blood_force);
    size_t i = 0;
    while (i < v.size()) {
      size_t j = v.find(',', i);
      if (j == std::string::npos) j = v.size();
      int n = std::atoi(v.substr(i, j - i).c_str());
      if (n >= 1 && n <= 7) out.push_back(n);
      i = j + 1;
    }
    return out;
  }();
  return z;
}

// Keep the forced zones' peak bleed points (dmg + (0x72 + region) * 4) at the
// forced level, for the region and its alt region (the getter picks one).
inline void Force(uint8_t* base, uint32_t dmg) {
  const auto& zones = ForcedZones();
  if (zones.empty()) return;
  float div = LoadF(base, kDivisor);
  if (!(div > 0)) return;
  uint32_t want = static_cast<uint32_t>(div * std::clamp(static_cast<float>(REXCVAR_GET(ufc3_blood_force_level)), 0.0f, 1.0f) + 0.999f);
  for (int z : zones) {
    for (int k = 0; k < 2; ++k) {
      int32_t region = static_cast<int32_t>(Load32(base, kZoneMap + z * 16 + k * 4));
      if (region < 0 || region > 10) continue;
      uint32_t addr = dmg + (0x72 + region) * 4;
      if (static_cast<int32_t>(Load32(base, addr)) < static_cast<int32_t>(want)) Store32(base, addr, want);
    }
  }
}

}  // namespace ufc3_blood

REX_EXTERN(__imp__sub_82C722C0);
REX_EXTERN(__imp__sub_8310E2F8);
REX_EXTERN(sub_82D50230);  // blood component SetLevel(this, region, level f1, delayed)
REX_EXTERN(sub_8310C8E0);  // liquid paste(this, name, only if not running)
REX_EXTERN(sub_8310B620);  // liquid: move an entry from the waiting list to idle

namespace ufc3_blood {
// Liquid map of a fighter the way sub_82D50840 finds it: blood component
// [fighter+0xEC8], its owner [blood+4], liquid [[owner+0x68C]+0x910].
inline uint32_t LiquidOf(uint8_t* base, uint32_t fighter) {
  uint32_t blood = fighter ? Load32(base, fighter + 0xEC8) : 0;
  if (!blood || Load32(base, blood) != kBloodVtable) return 0;
  uint32_t owner = Load32(base, blood + 4);
  uint32_t model = owner ? Load32(base, owner + 0x68C) : 0;
  return model ? Load32(base, model + 0x910) : 0;
}
// Hands on the Upper shader (DMG4b, FINDINGS-damage.md "DMG4b step 0"): zone 4
// skin meshes draw with a pixel shader that samples no liquid map, zone 2 with
// one that does. Called with each model file before the game parses it (JBOY,
// pointers relative to +8, mesh headers 184 bytes: +0 vertices, +112 ->uv,
// +124 shader, +144 zone, +148/+152 materials). A hand mesh has vertices inside
// the knuckle rects; on disc bodies the same mesh also holds a foot, which moves
// with it. Bare hands only.
constexpr uint32_t kMeshHdr = 184;
constexpr uint32_t kJboyTag = 0x4A424F59;

inline bool IsSkin(uint8_t* base, uint32_t h) {
  const char* sh = reinterpret_cast<const char*>(base + h + 124);
  return std::strncmp(sh, "yVFighterSkin", 13) == 0 && (sh[13] == 0 || (sh[13] == 'M' && sh[14] == 0));
}

// Note the model for the liquid built next, and give UFC 2009 ports the blood colour every
// other fighter has: g_f4BSSBldClr red 0.66 -> 0.55 (FINDINGS-blood-imbalance.md). Material
// block: table of offsets at mesh +152, count at +148; entry: name, u16 type at +16, value at +20.
constexpr float kBloodRed2009 = 0.66f, kBloodRedStock = 0.55f;
inline void NoteModel(uint8_t* base, uint32_t m) {
  if (!m || Load32(base, m) != kJboyTag) return;
  uint32_t size = Load32(base, m + 4);
  std::string id;
  uint32_t tc = Load32(base, m + 0x24), to = Load32(base, m + 0x2C);
  for (uint32_t i = 0; i < tc && i < 256 && to < size && size - to >= (i + 1) * 16; ++i) {
    const char* t = reinterpret_cast<const char*>(base + m + 8 + to + i * 16);
    std::string name(t, strnlen(t, 16));
    size_t cut = name.find("_skin");
    if (cut != std::string::npos && cut + 5 == name.size()) {
      id = name.substr(0, cut);
      break;
    }
  }
  if (id.empty()) return;
  uint32_t mc = Load32(base, m + 0x18), mo = Load32(base, m + 0x1C);
  bool found = false;
  if (mc && mc <= 512 && mo < size && size - mo >= mc * kMeshHdr) {
    for (uint32_t k = 0; k < mc; ++k) {
      uint32_t h = m + 8 + mo + k * kMeshHdr;
      if (!IsSkin(base, h)) continue;
      uint32_t count = Load32(base, h + 148), mats = Load32(base, h + 152);
      if (count > 64 || mats >= size || size - mats < count * 4) continue;
      for (uint32_t t = 0; t < count; ++t) {
        uint32_t e = Load32(base, m + 8 + mats + t * 4);
        if (e >= size || size - e < 24) continue;
        if (std::strncmp(reinterpret_cast<const char*>(base + m + 8 + e), "g_f4BSSBldClr", 16) != 0) continue;
        float red = LoadF(base, m + 8 + e + 20);
        if (std::abs(red - kBloodRed2009) < 0.01f) found = true;
        if (std::abs(red - kBloodRed2009) < 0.01f || std::abs(red - kBloodRedStock) < 0.01f) {
          // models load before hands are final, so gloved and bare get the same colour
          if (SettingMode() != kNormal) StoreF(base, m + 8 + e + 20, static_cast<float>(REXCVAR_GET(ufc3_blood_red)));  // models load before hands are final
        }
      }
    }
  }
  std::lock_guard<std::mutex> lock(g_map_mutex);
  if (found) g_ids_2009.insert(id);
  g_last_id = id;
  g_last_2009 = g_ids_2009.count(id) != 0;
}

inline void ZoneHands(uint8_t* base, uint32_t m) {
  if (!ufc3_pack::Active()) return;  // stock mode: models as on the disc
  NoteModel(base, m);
  const std::string& want = REXCVAR_GET(ufc3_knuckle_zone);
  // Not gated on bare hands: the R3 toggle comes after the select screen has loaded the fighters, so a model
  // loaded with gloves on would never get its liquid map on the hands (blood painted, none shown).
  if (want == "off" || !m || Load32(base, m) != kJboyTag) return;
  uint32_t size = Load32(base, m + 4);
  uint32_t mc = Load32(base, m + 0x18), mo = Load32(base, m + 0x1C);
  if (!mc || mc > 512 || mo >= size || size - mo < mc * kMeshHdr) return;
  std::string id;  // the fighter's id, from its "<id>_skin" texture name
  uint32_t tc = Load32(base, m + 0x24), to = Load32(base, m + 0x2C);
  for (uint32_t i = 0; i < tc && i < 256 && to < size && size - to >= (i + 1) * 16; ++i) {
    const char* t = reinterpret_cast<const char*>(base + m + 8 + to + i * 16);
    std::string name(t, strnlen(t, 16));
    size_t cut = name.find("_skin");
    if (cut != std::string::npos && cut + 5 == name.size()) {
      id = name.substr(0, cut);
      break;
    }
  }
  bool pick = want == "all" || want == id;
  auto mesh = [&](uint32_t k) { return m + 8 + mo + k * kMeshHdr; };
  auto is_hand = [&](uint32_t h) {
    uint32_t n = Load32(base, h), uv = Load32(base, h + 112);
    if (!n || n > 65535 || uv >= size || size - uv < n * 8) return false;
    uint32_t in = 0;
    for (uint32_t j = 0; j < n; ++j) {
      float u = LoadF(base, m + 8 + uv + 8 * j), v = LoadF(base, m + 8 + uv + 8 * j + 4);
      for (const auto& kn : kHandDetect) in += u >= kn.rect[0] && u <= kn.rect[2] && v >= kn.rect[1] && v <= kn.rect[3];
    }
    if (in >= 10 && REXCVAR_GET(ufc3_blood_trace)) {
      float b[4] = {1e9f, 1e9f, -1e9f, -1e9f};
      for (uint32_t j = 0; j < n; ++j) {
        float u = LoadF(base, m + 8 + uv + 8 * j), v = LoadF(base, m + 8 + uv + 8 * j + 4);
        b[0] = std::min(b[0], u), b[1] = std::min(b[1], v), b[2] = std::max(b[2], u), b[3] = std::max(b[3], v);
      }
      REXLOG_INFO("[blood] hand mesh {:08X}: {} verts, UV box {:.3f},{:.3f} - {:.3f},{:.3f}", h, n, b[0], b[1], b[2], b[3]);
    }
    return in >= 10;
  };
  auto has_textures = [&](uint32_t h) {  // the first material parameter is g_mDiffusePrm
    uint32_t mats = Load32(base, h + 152);
    if (!Load32(base, h + 148) || mats >= size || size - mats < 4) return false;
    uint32_t e = Load32(base, m + 8 + mats);
    return e < size && size - e >= 16 && std::strncmp(reinterpret_cast<const char*>(base + m + 8 + e), "g_mDiffusePrm", 16) == 0;
  };
  int hands = 0, moved = 0;
  for (uint32_t k = 0; k < mc;) {
    uint32_t h = mesh(k);
    if (!IsSkin(base, h) || Load32(base, h + 144) != 4 || !has_textures(h)) {
      ++k;
      continue;
    }
    // A textured mesh and the textureless ones after it are one run: a textureless
    // mesh in another zone than the mesh before it crashes the draw (SetTexture on
    // a material float), so a run with a hand moves whole.
    uint32_t end = k + 1;
    while (end < mc && IsSkin(base, mesh(end)) && Load32(base, mesh(end) + 144) == 4 && !has_textures(mesh(end))) ++end;
    int in_run = 0;
    for (uint32_t j = k; j < end; ++j) in_run += is_hand(mesh(j));
    hands += in_run;
    if (in_run && pick)
      for (uint32_t j = k; j < end; ++j) Store32(base, mesh(j) + 144, 2), ++moved;
    k = end;
  }
  // GitHub #5: meshes with the same material (+0x78, "handfeet") share one parameter block, laid out
  // for one technique (= zone). Ports have separate hand and foot meshes; a foot left in zone 4 drew
  // HandsFeet with the Upper block and bound a float as its cube-light texture (Roger Huerta
  // crashed, Danzig got a wrong bind that happened to be harmless). So the rest of a moved
  // material moves too, as the foot does on disc bodies, where it is the hand's own mesh.
  uint32_t tags[8];
  int ntags = 0;
  for (uint32_t k = 0; k < mc && ntags < 8; ++k) {
    uint32_t h = mesh(k), tag = Load32(base, h + 0x78);
    if (IsSkin(base, h) && Load32(base, h + 144) == 2 && is_hand(h) && std::find(tags, tags + ntags, tag) == tags + ntags)
      tags[ntags++] = tag;
  }
  for (uint32_t k = 0; k < mc && ntags; ++k) {
    uint32_t h = mesh(k);
    if (IsSkin(base, h) && Load32(base, h + 144) == 4 && std::find(tags, tags + ntags, Load32(base, h + 0x78)) != tags + ntags)
      Store32(base, h + 144, 2), ++moved;
  }
  if (REXCVAR_GET(ufc3_knuckle_trace)) {
    REXLOG_INFO("[knuckle] model {:08X}-{:08X} fighter '{}': {} meshes", m, m + 8 + size, id, mc);
    for (uint32_t k = 0; k < mc; ++k) {
      uint32_t h = mesh(k);
      REXLOG_INFO("[knuckle]   {:2} hdr {:08X} {:.16} zone {} v {} tag {} group {} w {} mats {} first '{:.16}' hand {}", k, h,
                  reinterpret_cast<const char*>(base + h + 124), Load32(base, h + 144), Load32(base, h),
                  Load32(base, h + 0x78), Load32(base, h + 0x60), Load32(base, h + 0x5C), Load32(base, h + 148),
                  has_textures(h) ? "g_mDiffusePrm" : "-", IsSkin(base, h) && is_hand(h) ? 1 : 0);
    }
  }
  if (hands && REXCVAR_GET(ufc3_blood_trace))
    REXLOG_INFO("[blood] model {:08X} fighter '{}': {} of {} meshes are hands, {} meshes moved to zone 2", m, id, hands, mc,
                moved);
}

inline bool Built(uint32_t liquid) {
  std::lock_guard<std::mutex> lock(g_map_mutex);
  auto it = g_maps.find(liquid);
  return it != g_maps.end() && it->second.buf;
}

// Paste the next knuckle layers on a fighter's liquid map (FINDINGS-liquid.md).
// A liquid runs at most 16 entries; the rest wait, first in first out. The borrowed
// face brushes hold at frame 90 and keep their slot until released, and stock only
// releases them at stage 1: 10 layers at once (35 entries, 28 holding) took every
// slot for the rest of the fight, so later face and chest pastes never drew (dmg4r).
// So: a few layers at a time, only with free slots, retired at the hold (below).
// An entry plays once per fight (a finished one stays at time > 0: only-if-idle skips
// it), so the layers are what makes knuckles grow. Returns false if no room yet.
constexpr int kKnuckleLayersPerPaste = 2;
constexpr uint32_t kKnuckleMaxRunning = 8;
inline bool PasteKnuckles(PPCContext& ctx, uint8_t* base, uint32_t fighter, int& layers) {
  uint32_t names = g_names.load();
  uint32_t liquid = LiquidOf(base, fighter);
  if (!names || !liquid || !Built(liquid)) return false;
  uint32_t running = Load32(base, liquid + 36), waiting = Load32(base, liquid + 48);
  bool room = waiting == 0 && running < kKnuckleMaxRunning;
  static std::atomic<int> blocked{0};  // called every frame while there's no room: log 1 in 60
  if (REXCVAR_GET(ufc3_blood_trace) && (room || ++blocked % 60 == 1))
    REXLOG_INFO("[blood] knuckle paste on fighter {:08X}: liquid {:08X} layers {} running {} waiting {} {}", fighter, liquid,
                layers, running, waiting, room ? "pasting" : "no room");
  if (!room) return false;
  for (int n = 0; n < kKnuckleLayersPerPaste && layers < kNumKnuckles; ++n, ++layers) {
    PPCContext c = ctx;
    c.r3.u64 = liquid;
    c.r4.u64 = names + 16 * layers;
    c.r5.u64 = 1;
    sub_8310C8E0(c, base);
  }
  return true;
}

// dmg4t: released flows run to frame 180 and cover the hand in a smear (dmg4s); the
// splatter look is the flow stopped at its frame-90 hold. The map keeps what was
// drawn, so end a knuckle entry when it reaches the hold: clear active and the
// liquid update moves it to idle (same path as a finished flow), freeing its slot.
inline void RetireHeldKnuckles(uint8_t* base, uint32_t liquid) {
  if (!liquid) return;
  int guard = 0;
  for (uint32_t e = Load32(base, liquid + 40); e && guard < 32; e = Load32(base, e + 8), ++guard) {
    if (std::memcmp(base + e + 0x18, "00_lhand", 8) && std::memcmp(base + e + 0x18, "00_rhand", 8)) continue;
    uint32_t f = Load32(base, e + 60);
    if (f & 0x40000000) Store32(base, e + 60, f & 0x7FFFFFFF);
  }
}

// dmg4t: a face builds up by painting more of it, not by repainting (an entry plays
// once per fight and the same image doesn't add up, FINDINGS-liquid.md): every 3 s
// the next region's flow (ba0, released so it runs) and bb0 stamp, the bleeding side
// first; a bleeding region gets its own bb0. Light cuts stay local, heavy covers the
// face. Region table: 0x38 per region, +4 ba0 name, +20 bb0 name (bb1 is the pale one).
constexpr uint32_t kRegionTable = 0x83336740;
constexpr int kFaceOrder[2][12] = {{1, 3, 9, 7, 5, 6, 0, 18, 8, 10, 2, 4}, {2, 4, 10, 8, 5, 6, 18, 0, 7, 9, 1, 3}};
constexpr int kChestAfterFaceRegions = 8;  // dmg4z: 6 let the chest start with one side of the face light
inline void PasteName(PPCContext& ctx, uint8_t* base, uint32_t liquid, uint32_t name, bool release) {
  if (!name) return;
  PPCContext c = ctx;
  c.r3.u64 = liquid;
  c.r4.u64 = name;
  c.r5.u64 = 1;
  sub_8310C8E0(c, base);
  if (!release) return;
  c = ctx;
  c.r3.u64 = liquid;
  c.r4.u64 = name;
  sub_8310BF48(c, base);
}
inline void BuildFace(PPCContext& ctx, uint8_t* base, State& st, uint32_t liquid, float raw_worst) {
  if (!liquid || st.frame - st.face_tick < 180) return;
  // dmg4z33: was >= 12: the game's re-flows kept it there and the face stopped building (chi3x-z32-ufc 2:30-5:00, no chest).
  if (Load32(base, liquid + 48) > 0 || Load32(base, liquid + 36) >= 15) return;
  st.face_tick = st.frame;
  int worst = 0;
  for (int r = 0; r <= 10; ++r)
    if (st.raw[r] > st.raw[worst]) worst = r;
  if (st.face_side < 0) st.face_side = (worst == 1 || worst == 3 || worst == 7 || worst == 9 || worst == 0) ? 0 : 1;
  for (int r = 0; r <= 10; ++r) {
    if (st.raw[r] >= 0.1f) st.bled |= 1u << r;
    if (st.raw[r] < 0.1f || (st.face_stamped & (1u << r))) continue;
    st.face_stamped |= 1u << r;
    PasteName(ctx, base, liquid, Load32(base, kRegionTable + 0x38 * r + 20), false);
  }
  // dmg4z3: cuts late in a fight stayed light (first cut to heavy took 92 s, dmg4z2 fight 3):
  // the face grows from a lower level. The chest keeps its own heavy gate.
  // dmg4z33: the regions follow the bleed: a full face at 0.5, not 0.2 (a mask stood a minute into a fight, 36 s after raw 0.2).
  int limit = std::min(12, static_cast<int>(raw_worst * 24.0f));
  if (st.face_added >= limit) {
    // dmg4z: every region painted: thicken them, bleeding side first, two per step.
    uint32_t names = g_thick_names.load();
    // dmg4z33: thick only on a heavy bleed. Soft brushes: one layer, the cut's side first, the far side at 0.7; a second layer
    // at 0.85, no third: stacked thin runs turn into a hairy tangle (look-z43; user: no goop; the cut's side bleeds more).
    const bool soft = REXCVAR_GET(ufc3_blood_soft) && SoftBrushesInstalled();
    int layers = !soft ? kThickLayers : raw_worst >= 0.85f ? 2 : 1;
    int thick_limit = raw_worst >= 0.5f ? limit * layers : 0;
    for (int n = 0; names && limit && n < 2 && st.thick_added < thick_limit; ++n, ++st.thick_added) {
      int reg = kFaceOrder[st.face_side][st.thick_added % limit];
      const bool far_side = st.face_side == 0 ? (reg == 8 || reg == 10 || reg == 2 || reg == 4) : (reg == 7 || reg == 9 || reg == 1 || reg == 3);
      if (soft && far_side && raw_worst < 0.7f) break;
      PasteName(ctx, base, liquid, names + 16 * (ThickSource(reg) * kThickLayers + st.thick_added / limit), true);
    }
    return;
  }
  int r = kFaceOrder[st.face_side][st.face_added++];
  PasteName(ctx, base, liquid, Load32(base, kRegionTable + 0x38 * r + 4), true);
  if (!(st.face_stamped & (1u << r))) {
    st.face_stamped |= 1u << r;
    PasteName(ctx, base, liquid, Load32(base, kRegionTable + 0x38 * r + 20), false);
  }
}

// dmg4q: the chest's own timed path pastes "bb0" brushes, which have no .yda in
// liquid.pac (no entries), so it never painted (dmg4p heavy: one ba0 paste at
// fight start, then nothing). Paste the chest ba0 brushes on the fighter's own
// liquid ourselves.
constexpr const char* kChestBrushes[] = {"00_cchest_ba0", "00_lchest_ba0", "00_rchest_ba0"};
inline std::atomic<uint32_t> g_chest_names{0};
inline void PasteChest(PPCContext& ctx, uint8_t* base, uint32_t fighter, int& layer) {
  uint32_t names = g_chest_names.load();
  if (!names) {
    uint32_t n = REX_KERNEL_MEMORY()->SystemHeapAlloc(16 * 3);
    if (!n) return;
    std::memset(base + n, 0, 16 * 3);
    for (int k = 0; k < 3; ++k) std::memcpy(base + n + 16 * k, kChestBrushes[k], std::strlen(kChestBrushes[k]));
    uint32_t want = 0;
    if (g_chest_names.compare_exchange_strong(want, n)) names = n;
    else { REX_KERNEL_MEMORY()->SystemHeapFree(n); names = want; }
  }
  uint32_t liquid = LiquidOf(base, fighter);
  if (!liquid) return;
  // dmg4z: the first paste is the ba0 flows, each later one a thick layer (a repeat of
  // ba0 is a no-op). A thick layer waits until the liquid has room.
  uint32_t thick = g_thick_names.load();
  constexpr int kChestThickLayers = 1;  // user 2026-11-05: three thick layers drenched the chest
  // FINDINGS-blood-imbalance.md: the UFC 2009 skin atlas packs more body into a chest rect
  // (Tim 2269 vs a typical disc fighter 1221), so those bodies get the ba0 flows only.
  bool is_2009;
  {
    std::lock_guard<std::mutex> lock(g_map_mutex);
    auto it = g_maps.find(liquid);
    is_2009 = it != g_maps.end() && it->second.is_2009;
  }
  // User: a few drops, then instantly big streaks. Soft brushes: specks and smear first, then one flow at a time (rchest's
  // forked flow never: see the paste hook). The game's own run-down waits for the same stages (sub_82D50230 hook).
  const bool stages = !is_2009 && thick && REXCVAR_GET(ufc3_blood_soft) && SoftBrushesInstalled();
  auto paste = [&](uint32_t name) {
    PPCContext c = ctx;
    c.r3.u64 = liquid;
    c.r4.u64 = name;
    c.r5.u64 = 1;
    sub_8310C8E0(c, base);
  };
  if (stages) {
    if (layer > 2) return;
    if (layer == 0) {
      if (Load32(base, liquid + 48) > 0 || Load32(base, liquid + 36) >= 8) return;
      if (REXCVAR_GET(ufc3_blood_chest_flows))
        for (int k = 0; k < 3; ++k) paste(thick + 16 * ((kThickChestSrc + k) * kThickLayers));
      else
        paste(thick + 16 * (kThickChestSrc * kThickLayers));
    } else {
      if (REXCVAR_GET(ufc3_blood_chest_flows)) paste(names + 16 * (layer - 1));  // cchest, then lchest
      else paste(thick + 16 * ((kThickChestSrc + layer) * kThickLayers));         // the other two soft brushes
    }
    ++layer;
    return;
  }
  if (layer > (is_2009 ? 0 : kChestThickLayers)) return;
  if (layer > 0 && (!thick || Load32(base, liquid + 48) > 0 || Load32(base, liquid + 36) >= 8)) return;
  for (int k = 0; k < 3; ++k) paste(layer == 0 ? names + 16 * k : thick + 16 * ((kThickChestSrc + k) * kThickLayers + layer - 1));
  ++layer;
}
}  // namespace ufc3_blood

// Blood Reset (vtable slot 0x20): clears the liquid map and zeroes the blood state.
// The fight-end flow calls it for every fighter at phases 214/314 (0x82CF0E14) and
// 216/316 (0x82CEAF74), so blood was gone by the verdict scene, although that scene
// uses the same fighters and liquid (FINDINGS-liquid.md). dmg4x: at those two calls zero the
// state as stock but keep the map, and clear it at the fighter's next face update (a
// rematch must start clean). Round clears (phases 105/107/114) stay stock.
REX_EXTERN(sub_8310BD08);
REX_EXTERN(__imp__sub_82D50490);
namespace ufc3_blood {
inline uint32_t g_kept[10] = {};  // fighters whose map survived the fight end
inline void MarkKept(uint32_t fighter) {
  for (uint32_t& k : g_kept)
    if (k == fighter) return;
  for (uint32_t& k : g_kept)
    if (!k) {
      k = fighter;
      return;
    }
}
inline bool TakeKept(uint32_t fighter) {
  for (uint32_t& k : g_kept)
    if (k == fighter) {
      k = 0;
      return true;
    }
  return false;
}
inline void ResetProgress(State& st) {
  std::fill(std::begin(st.ramp), std::end(st.ramp), 0.0f);
  st.knuckle_layers = 0;
  st.knuckle_step = 0;
  st.face_added = 0;
  st.face_stamped = 0;
  st.face_side = -1;
  st.chest_frame = 0;
  st.thick_added = 0;
  st.chest_layers = 0;
  st.bled = 0;
}
// dmg4z: the game washes a fighter at the corner (0x822ABC78, per fighter as they sit) and
// again as the next round starts (0x82CF0E18 r27 114, then 0x82C6F6F0). Stock leaves them
// spotless. Leave what bled showing faded: the pale bb1 stamps of the regions that had blood.
inline void PasteWipe(PPCContext& ctx, uint8_t* base, uint32_t liquid) {
  uint32_t names = g_wipe_names.load();
  if (!liquid || !names || !REXCVAR_GET(ufc3_blood_soft) || !SoftBrushesInstalled()) return;
  // z49-z50 (user: only the winner looked wiped): at the verdict the 16 slots can be full and 100+ entries queued (FIFO, no cap, one
  // promoted per update as slots free), held and lying-wait flows never advance, and flows still running paint over the wipe. So the
  // wipe goes first: queued entries (never drew) back to idle, every running entry ended (the map keeps what it drew, the next update
  // moves it to idle, then promotes the wipe from the waiting list: sub_8310CB00 retires before it promotes).
  uint32_t running = Load32(base, liquid + 36), waiting = Load32(base, liquid + 48);
  int blood = 0, sweat = 0, held = 0, lying_wait = 0, guard = 0;
  for (uint32_t e = Load32(base, liquid + 40); e && guard < 32; e = Load32(base, e + 8), ++guard) {
    uint32_t f = Load32(base, e + 60);
    (Load32(base, e + 0x14) == 0 ? blood : sweat)++;
    if (f & 0x40000000) ++held;
    if (f & 0x20000000) ++lying_wait;
    Store32(base, e + 60, f & 0x7FFFFFFF);
  }
  guard = 0;
  for (uint32_t e = Load32(base, liquid + 52); e && guard < 4096; ++guard) {
    uint32_t next = Load32(base, e + 8);
    Store32(base, e + 60, Load32(base, e + 60) & 0x7FFFFFFF);
    PPCContext c = ctx;
    c.r3.u64 = liquid;
    c.r4.u64 = e;
    sub_8310B620(c, base);
    e = next;
  }
  if (REXCVAR_GET(ufc3_blood_trace))
    REXLOG_INFO("[blood] wipe liquid {:08X}: running {} (blood {} sweat {} held {} lying-wait {}) waiting {} -> {} lying {}", liquid, running, blood,
                sweat, held, lying_wait, waiting, Load32(base, liquid + 48), Load32(base, liquid + 84));
  for (int w = 0; w < kNumWipe; ++w) PasteName(ctx, base, liquid, names + 16 * w, false);
}
inline void PasteResidue(PPCContext& ctx, uint8_t* base, uint32_t liquid, uint32_t mask) {
  if (!liquid) return;
  for (int r = 0; r <= 13; ++r)
    if (mask & (1u << r)) PasteName(ctx, base, liquid, Load32(base, kRegionTable + 0x38 * r + 24), false);
}
}  // namespace ufc3_blood
extern "C" REX_FUNC(sub_82D50490) {
  if (!ufc3_pack::Active()) {  // stock mode: the original, untouched
    __imp__sub_82D50490(ctx, base);
    return;
  }
  namespace bl = ufc3_blood;
  uint32_t lr = static_cast<uint32_t>(ctx.lr), blood = ctx.r3.u32;
  bool end_of_fight = (lr == 0x82CF0E18 && ctx.r27.u32 != 114) || lr == 0x82CEAF78;
  bool keep = end_of_fight && (bl::GetMode() != bl::kNormal || bl::KnucklesOn());
  uint32_t fighter = bl::Load32(base, blood + 4);
  if (REXCVAR_GET(ufc3_blood_trace))
    REXLOG_INFO("[blood] reset blood {:08X} fighter {:08X} lr {:08X} r27 {} liquid {:08X} -> {}", blood, fighter, lr, ctx.r27.u32,
                bl::LiquidOf(base, fighter), keep ? "map kept" : "stock clear");
  if (!keep) {
    // dmg4y: a stock clear (round break) empties the map and makes every entry playable
    // again, so our progress must start over too, or the chest comes back with the face
    // not rebuilt (dmg4x round 2).
    bool corner = lr == 0x822ABC78 || (lr == 0x82CF0E18 && ctx.r27.u32 == 114) || lr == 0x82C6F6F0;
    uint32_t residue = 0;
    for (auto& st : bl::g_states)
      if (st.blood == blood) {
        if (corner) {
          // dmg4z3 (user: lighter): only regions that really bled, not all the build-up
          // painted, and only the centre of the chest.
          st.residue |= st.bled;
          if (st.chest_layers > 0) st.residue |= 1u << 11;
          residue = st.residue;
        } else {
          st.residue = 0;
        }
        bl::ResetProgress(st);
      }
    PPCContext saved = ctx;
    __imp__sub_82D50490(ctx, base);
    if (residue && REXCVAR_GET(ufc3_blood_stain) && bl::GetMode() != bl::kNormal) {
      if (REXCVAR_GET(ufc3_blood_trace)) REXLOG_INFO("[blood] fighter {:08X}: corner wash, residue mask {:05X}", fighter, residue);
      bl::PasteResidue(saved, base, bl::LiquidOf(base, fighter), residue);
    }
    return;
  }
  std::memset(base + blood + 8, 0, 76);
  std::memset(base + blood + 84, 0, 152);
  bl::MarkKept(fighter);
  if (bl::GetMode() != bl::kNormal) bl::PasteWipe(ctx, base, bl::LiquidOf(base, fighter));  // only-if-idle: one paste per fight
}

// Every blood SetLevel (trace): who sets which region (chest flows: who starts them).
REX_EXTERN(__imp__sub_82D50230);
extern "C" REX_FUNC(sub_82D50230) {
  if (!ufc3_pack::Active()) {  // stock mode: the original, untouched
    __imp__sub_82D50230(ctx, base);
    return;
  }
  namespace bl = ufc3_blood;
  // dmg4o: the blood component itself sets a chest region (12 seen) to the face
  // level (caller 0x82D50820), so a strong face bleed put blood on the chest at
  // once. User: chest only after a lot of face bleeding -> hold those calls back
  // until this fighter's unscaled face level is heavy. Stock (normal) untouched.
  uint32_t reg = ctx.r4.u32;
  int mode = bl::GetMode();
  if (mode != bl::kNormal && reg >= 11 && reg <= 17 && static_cast<uint32_t>(ctx.lr) == 0x82D50820) {
    // dmg4u: same gate as our own chest paste: heavy AND the face built up (the run-down
    // put a chest streak 2 s into a forced heavy fight, before the face had any blood).
    float heavy = mode == bl::kMax ? 0.45f : 0.55f, face = 0;
    int added = 0, layers = 0;
    for (const auto& st : bl::g_states)
      if (st.blood == ctx.r3.u32) {
        face = std::max(face, st.face_raw);
        added = std::max(added, st.face_added);
        layers = std::max(layers, st.chest_layers);
      }
    // Paced chest: a region's flow waits for its stage in PasteChest (11 = 2, 12 = 3).
    bool staged = REXCVAR_GET(ufc3_blood_soft) && bl::SoftBrushesInstalled() && layers < 3 && static_cast<int>(reg) - 9 > layers;
    if (face < heavy || added < bl::kChestAfterFaceRegions || staged) {
      if (REXCVAR_GET(ufc3_blood_trace))
        REXLOG_INFO("[blood] chest run-down region {} held back (face raw {:.3f} < {:.2f})", reg, face, heavy);
      return;
    }
  }
  if (REXCVAR_GET(ufc3_blood_trace)) {
    uint32_t r = ctx.r4.u32;
    static std::atomic<int> n{0};
    int c = ++n;
    if (r >= 11 || c <= 300 || c % 50 == 0)
      REXLOG_INFO("[blood] setlevel blood {:08X} region {} level {:.3f} delayed {} lr {:08X}", ctx.r3.u32, r, ctx.f1.f64,
                  ctx.r6.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82D50230(ctx, base);
}

// Every liquid paste (trace): which brush, on which liquid, and from where.
// dmg4z31: each liquid keeps 2 bits per entry per group, and sub_8310B4F0 copies them out as many bytes as the liquid says
// (liquid+144+4*group). Its two callers give it 64-byte buffers: dmg+0x188 (the peak bleed points start right after it) and
// blood+76. Stock maps fit; our thick and knuckle entries (757) spilled over the peak points, which then read as level 1.
// Copy at most what the buffers hold. Entries past the first 256 lose only this saved state.
REX_EXTERN(__imp__sub_8310B4F0);
extern "C" REX_FUNC(sub_8310B4F0) {
  if (!ufc3_pack::Active()) {  // stock mode: the original, untouched
    __imp__sub_8310B4F0(ctx, base);
    return;
  }
  namespace bl = ufc3_blood;
  uint32_t size_addr = ctx.r3.u32 + (ctx.r4.u32 + 36) * 4;
  uint32_t size = bl::Load32(base, size_addr);
  if (size <= 64) {
    __imp__sub_8310B4F0(ctx, base);
    return;
  }
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) REXLOG_INFO("[blood] liquid state save limited: {} bytes to 64", size);
  bl::Store32(base, size_addr, 64);
  __imp__sub_8310B4F0(ctx, base);
  bl::Store32(base, size_addr, size);
  // The whole state stays on the host, found again by its first 64 bytes when it is loaded.
  uint32_t state = bl::Load32(base, size_addr + 8);
  if (!state || size > 4096) return;
  bl::SavedState s;
  std::memcpy(s.head, base + state, 64);
  s.full.assign(base + state, base + state + size);
  std::lock_guard<std::mutex> lock(bl::g_saved_mutex);
  auto& list = bl::g_saved[(uint64_t(size_addr) << 8)];
  if (!list.empty() && list.back().full == s.full) return;
  list.push_back(std::move(s));
  if (list.size() > 512) list.erase(list.begin());
}
// The load (replays): same buffers, same size field. Reading all 92 bytes pulled 28 bytes of whatever follows the
// buffer into the state, and every set bit starts that entry, so replays showed chest and face layers never painted.
// Limiting it to 64 dropped our thick and knuckle layers instead (replays looked lighter than the fight), so the
// load uses the host's full copy, and then does what the original does after its copy: sub_8310CBC8 restarts the entries.
REX_EXTERN(__imp__sub_8310CDB0);
REX_EXTERN(sub_8310CBC8);
extern "C" REX_FUNC(sub_8310CDB0) {
  if (!ufc3_pack::Active()) {  // stock mode: the original, untouched
    __imp__sub_8310CDB0(ctx, base);
    return;
  }
  namespace bl = ufc3_blood;
  uint32_t liquid = ctx.r3.u32, src = ctx.r5.u32;
  uint32_t size_addr = liquid + (ctx.r4.u32 + 36) * 4;
  uint32_t size = bl::Load32(base, size_addr);
  if (REXCVAR_GET(ufc3_blood_trace))
    REXLOG_INFO("[blood] liquid state load: liquid {:08X} group {} size {} src {:08X} running {} waiting {} lr {:08X} {}", liquid, ctx.r4.u32, size, src,
                bl::Load32(base, liquid + 36), bl::Load32(base, liquid + 48), static_cast<uint32_t>(ctx.lr),
                REXCVAR_GET(ufc3_blood_replay_keep) ? "kept" : "restored");
  // z51 (user: replays showed cuts and no blood): the restore (sub_8310CBC8) clears the map and restarts every saved entry from frame 0, 16 at a
  // time at up to 3 s each, so a short replay ends before the blood is back, while cuts live on their own layer. Keep the map as it is.
  if (REXCVAR_GET(ufc3_blood_replay_keep)) {
    ctx.r3.u64 = 1;
    return;
  }
  if (size <= 64) {
    __imp__sub_8310CDB0(ctx, base);
    return;
  }
  uint32_t state = bl::Load32(base, size_addr + 8);
  bool found = false;
  if (src && state) {
    std::lock_guard<std::mutex> lock(bl::g_saved_mutex);
    auto& list = bl::g_saved[(uint64_t(size_addr) << 8)];
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
      if (it->full.size() == size && std::memcmp(it->head, base + src, 64) == 0) {
        std::memcpy(base + state, it->full.data(), size);
        found = true;
        break;
      }
    }
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8) REXLOG_INFO("[blood] liquid state load: {} bytes, full copy {}", size, found ? "found" : "not found");
  if (found) {
    ctx.r3.u64 = liquid;
    sub_8310CBC8(ctx, base);
    ctx.r3.u64 = 1;
    return;
  }
  bl::Store32(base, size_addr, 64);
  __imp__sub_8310CDB0(ctx, base);
  bl::Store32(base, size_addr, size);
}
REX_EXTERN(__imp__sub_8310C8E0);
extern "C" REX_FUNC(sub_8310C8E0) {
  if (!ufc3_pack::Active()) {  // stock mode: the original, untouched
    __imp__sub_8310C8E0(ctx, base);
    return;
  }
  if (REXCVAR_GET(ufc3_blood_trace)) {
    uint32_t nm = ctx.r4.u32;
    char name[17] = {};
    if (nm) std::memcpy(name, base + nm, 16);
    REXLOG_INFO("[blood] paste {} on liquid {:08X} (only-if-idle {}, lr {:08X})", name, ctx.r3.u32, ctx.r5.u32,
                static_cast<uint32_t>(ctx.lr));
  }
  // rchest's flow forks into a stick figure, the last vein on the chest (FINDINGS-blood-look.md option 4).
  namespace bl = ufc3_blood;
  if (ctx.r4.u32 && bl::GetMode() != bl::kNormal && REXCVAR_GET(ufc3_blood_soft) && bl::SoftBrushesInstalled() &&
      (std::strncmp(reinterpret_cast<const char*>(base + ctx.r4.u32), "00_rchest_ba0", 16) == 0 ||
       (!REXCVAR_GET(ufc3_blood_chest_flows) && (std::strncmp(reinterpret_cast<const char*>(base + ctx.r4.u32), "00_cchest_ba0", 16) == 0 ||
                                                 std::strncmp(reinterpret_cast<const char*>(base + ctx.r4.u32), "00_lchest_ba0", 16) == 0)))) {
    bool is_2009 = false;
    {
      std::lock_guard<std::mutex> lock(bl::g_map_mutex);
      auto it = bl::g_maps.find(ctx.r3.u32);
      is_2009 = it != bl::g_maps.end() && it->second.is_2009;
    }
    if (!is_2009) return;
  }
  __imp__sub_8310C8E0(ctx, base);
}

// The liquid builder: knuckle rects in, knuckle names on their entries after.
extern "C" REX_FUNC(sub_8310E2F8) {
  if (!ufc3_pack::Active()) {  // stock mode: the original, untouched
    __imp__sub_8310E2F8(ctx, base);
    return;
  }
  namespace bl = ufc3_blood;
  uint32_t liquid = ctx.r3.u32;
  uint32_t map = REXCVAR_GET(ufc3_knuckle_blood) || bl::GetMode() != bl::kNormal ? bl::Load32(base, liquid + 0x68) : 0;
  uint32_t copy = map ? bl::ExtendMap(base, liquid, map) : 0;
  if (copy) bl::Store32(base, liquid + 0x68, copy);  // stays: the entries point at its rects
  __imp__sub_8310E2F8(ctx, base);
  if (copy) bl::RenameKnuckles(base, liquid, copy);
}

// 0x82C72670: f1 = zone level 0..1 for region r31 of damage object r26.
void ufc3_blood_level(PPCRegister& f1, PPCRegister& r26, PPCRegister& r31) {
  if (!ufc3_pack::Active()) return;  // stock mode: stock level
  namespace bl = ufc3_blood;
  int mode = bl::GetMode();
  bl::State* s = bl::g_cur;
  int region = static_cast<int>(r31.u32);
  float lift = bl::GlovedLift();
  if (!s || s->dmg != r26.u32 || region < 0 || region >= bl::kRegions) {  // the stock update keeps no state
    f1.f64 = std::min(1.0, f1.f64 * lift);
    return;
  }
  float in = static_cast<float>(f1.f64);
  // Optional limit on how fast a level rises (off; the old one-frame jumps were the dmg4z31 overflow). Called once per frame.
  double rise = REXCVAR_GET(ufc3_blood_max_rise);
  if (mode == bl::kMax && rise > 0 && in > 0) {
    s->ramp[region] = std::min(in, s->ramp[region] + static_cast<float>(rise / 3600.0));
    in = s->ramp[region];
  }
  float out = std::min(1.0f, in * lift);
  // User 2026-11-04 (dmg4k): a cut must show when it opens (floor), but the drips
  // that build up on the chest only come with a heavy face bleed (re-flow below).
  if (mode == bl::kMore) {
    out = in > 0 ? std::min(1.0f, std::max(in * 1.6f, 0.4f)) : 0.0f;
  } else if (mode == bl::kMax) {
    out = in > 0 ? std::min(1.0f, std::max(in * static_cast<float>(REXCVAR_GET(ufc3_blood_max_gain)), static_cast<float>(REXCVAR_GET(ufc3_blood_max_floor)))) : 0.0f;
  }
  s->own[region] = out;
  s->raw[region] = in;
  if (mode != bl::kNormal) {
    float k = mode == bl::kMax ? 0.8f : 0.5f;
    for (const auto& sp : bl::kSpread)  // run-down to the next face zone: only once the source bleeds for real
      if (sp.to == region && std::max(s->raw[sp.from], s->prev_raw[sp.from]) >= 0.1f)
        out = std::max(out, s->prev[sp.from] * k);
  }
  if (REXCVAR_GET(ufc3_blood_trace) && std::abs(out - s->traced[region]) > 0.001f) {
    s->traced[region] = out;
    static std::atomic<int> n{0};
    int c = ++n;
    if (c <= 200 || c % 100 == 0)
      REXLOG_INFO("[blood] dmg {:08X} region {} level {:.3f} -> {:.3f} (mode {}, frame {})", s->dmg, region, in, out,
                  mode, s->frame);
  }
  f1.f64 = out;
}

// The face blood update itself: force first, then the stock loop (with the hook
// above), then the chest from the worst face region, then the opponent's knuckles.
extern "C" REX_FUNC(sub_82C722C0) {
  if (!ufc3_pack::Active()) {  // stock mode: the original, untouched
    __imp__sub_82C722C0(ctx, base);
    return;
  }
  namespace bl = ufc3_blood;
  uint32_t dmg = ctx.r3.u32;
  int mode = bl::GetMode();
  bool trace = REXCVAR_GET(ufc3_blood_trace);
  bool knuckles = bl::KnucklesOn();
  if (mode == bl::kNormal && !trace && !knuckles && bl::ForcedZones().empty()) {
    __imp__sub_82C722C0(ctx, base);
    return;
  }
  bl::Force(base, dmg);
  bl::State& s = bl::StateFor(dmg);
  {
    uint32_t kept_fighter = bl::Load32(base, dmg);
    if (kept_fighter && bl::TakeKept(kept_fighter)) {
      uint32_t lq = bl::LiquidOf(base, kept_fighter);
      if (trace) REXLOG_INFO("[blood] fighter {:08X}: first face update after the fight end, clearing kept map {:08X}", kept_fighter, lq);
      if (lq) {
        PPCContext c = ctx;
        c.r3.u64 = lq;
        sub_8310BD08(c, base);
      }
      bl::ResetProgress(s);
      s.residue = 0;
    }
  }
  std::memcpy(s.prev, s.own, sizeof(s.own));
  std::memset(s.own, 0, sizeof(s.own));
  std::memcpy(s.prev_raw, s.raw, sizeof(s.raw));
  std::memset(s.raw, 0, sizeof(s.raw));
  bl::g_cur = &s;
  __imp__sub_82C722C0(ctx, base);
  bl::g_cur = nullptr;
  ++s.frame;

  uint32_t fighter = bl::Load32(base, dmg);
  uint32_t blood = fighter ? bl::Load32(base, fighter + 0xEC8) : 0;
  if (!blood || bl::Load32(base, blood) != bl::kBloodVtable) return;

  float worst = 0;
  for (int r = 0; r <= 10; ++r) worst = std::max(worst, s.own[r]);
  float chest = 0;
  // User 2026-11-04: chest blood is progressive, not timed: none until the face
  // bleeds a lot, then it follows how heavy the face bleed is. Uses the face's own
  // (unscaled) level; the scaled one has a floor (a cut shows at once), so it can't be the gate.
  float raw_worst = 0;
  for (int r = 0; r <= 10; ++r) raw_worst = std::max(raw_worst, s.raw[r]);
  s.blood = blood;
  // dmg4z2: in a real fight the raw level reads zero for single frames about once a second
  // (dmg4z-defaults: 360 "new fight" resets in 7 minutes, so the face never built past its
  // first region). Only a long zero counts as zero.
  constexpr uint32_t kRawZeroFrames = 300;
  if (raw_worst > 0) {
    s.raw_held = raw_worst;
    s.raw_zero = 0;
  } else if (++s.raw_zero < kRawZeroFrames) {
    raw_worst = s.raw_held;
  } else {
    s.raw_held = 0;
  }
  // dmg4w: the same damage object lives on through a rematch and the game clears the
  // map (every entry can play again), but our progress stayed: knuckle layers at 10 and
  // face regions at 11, so fight 3 got no knuckles or face build-up and an instant chest
  // (dmg4v log). Peak bleed only drops to zero at a new fight: start over there.
  if (raw_worst <= 0 && s.face_raw > 0) {
    bl::ResetProgress(s);
    s.residue = 0;
    if (trace) REXLOG_INFO("[blood] dmg {:08X}: bleed back to zero, blood progress reset (new fight)", dmg);
  }
  s.face_raw = raw_worst;
  bool face_shown = (bl::Load32(base, bl::kDisplayFlags) & 0x30) != 0;
  if (mode != bl::kNormal && face_shown) {
    // One value below 0.5 (the red bb0 stage; DMG4: the chest stays red, and a change
    // would restart the 30 delay). max starts a little earlier than more.
    chest = raw_worst >= (mode == bl::kMax ? 0.45f : 0.55f) ? 0.25f : 0.0f;
  }
  // dmg4t (user: chest only once the face is pretty bloody): build the face up first,
  // the chest waits until enough of it is painted.
  const bool paused = bl::PaintPaused();
  if (mode != bl::kNormal && face_shown && !paused) bl::BuildFace(ctx, base, s, bl::LiquidOf(base, fighter), raw_worst);
  if (s.face_added < bl::kChestAfterFaceRegions) chest = 0;
  if (chest > 0) {
    // dmg4q: own ba0 pastes (the delayed SetLevel path pastes bb0 = nothing).
    // Progressive: the heavier the face bleed, the more often the chest re-flows
    // (max: 8 s at the gate ... 3 s at full; more is 1.5x slower).
    float over = std::clamp((raw_worst - 0.45f) / 0.55f, 0.0f, 1.0f);
    uint32_t period = static_cast<uint32_t>(480 - over * 240);
    if (mode == bl::kMore) period = period * 3 / 2;
    if (!paused && (s.chest_frame == 0 || s.frame - s.chest_frame >= period)) {
      s.chest_frame = s.frame;
      if (trace) {
        uint32_t lq = bl::LiquidOf(base, fighter);
        REXLOG_INFO("[blood] chest paste fighter {:08X} raw {:.3f} period {} running {} waiting {}", fighter, raw_worst, period,
                    lq ? bl::Load32(base, lq + 36) : 0, lq ? bl::Load32(base, lq + 48) : 0);
      }
      bl::PasteChest(ctx, base, fighter, s.chest_layers);
    }
  } else {
    s.chest_frame = 0;
  }
  if (knuckles) {
    bl::RetireHeldKnuckles(base, bl::LiquidOf(base, fighter));
    uint32_t opp = bl::Opponent(fighter, bl::LiquidOf(base, fighter) != 0);
    int step = worst > 0.1f ? 1 + static_cast<int>(raw_worst * 20) : 0;
    uint32_t period = mode == bl::kMax ? 300 : mode == bl::kMore ? 450 : 600;  // dmg4i: fast re-pastes = streaks
    bool shown = (bl::Load32(base, bl::kDisplayFlags) & 0x30) != 0;
    if (!step) {
      s.knuckle_step = 0;
    } else if (opp && shown && (step > s.knuckle_step || s.frame - s.knuckle_frame >= period)) {
      if (bl::PasteKnuckles(ctx, base, opp, s.knuckle_layers)) {
        s.knuckle_step = std::max(step, s.knuckle_step);
        s.knuckle_frame = s.frame;
      }
    }
  }
  if (trace && (chest != s.chest || s.frame % 120 == 0)) {
    s.chest = chest;
    std::string lv;
    for (int r = 0; r < bl::kRegions; ++r)
      if (s.own[r] > 0) lv += " " + std::to_string(r) + "=" + std::to_string(s.own[r]).substr(0, 5);
    // Chest region 11: pending level / timer at blood + 8 * (11 + 11) - 4 / + 0.
    REXLOG_INFO("[blood] dmg {:08X} fighter {:08X} frame {} worst {:.3f} raw {:.3f} chest {:.2f} (pending {:.2f} timer {:.1f}) levels{}",
                dmg, fighter, s.frame, worst, raw_worst, chest, bl::LoadF(base, blood + 0xAC), bl::LoadF(base, blood + 0xB0),
                lv.empty() ? " none" : lv);
  }
}
