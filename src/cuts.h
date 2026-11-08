// ufc3 - gameplay cuts and bleeding (FINDINGS-damage.md "## DMG5", item DMG6).
// These change the fight itself, unlike ufc3_blood (visual only, src/blood.h).
//
// sub_82C73028 (OnHit: damage object, attacker, hit, zone 0..7) fills the zone's
// meter, then sub_82C72858 rolls the cut for zones 1-7 (chance from sub_82C70588)
// and sub_82C71A70 opens the forehead from its meter alone. A new cut goes through
// StartBleed sub_82C6F700: a delay, then sub_82C722C0 adds bleed points every
// interval; hits on an open cut add more (sub_82C71FB8).
//
// Two rates each (user 2026-11-05: gloved slightly above the original, bare knuckle is the
// bloody mode): ufc3_cut_rate / ufc3_bleed_rate for gloves, ufc3_bk_cut_rate /
// ufc3_bk_bleed_rate for bare hands and wraps.
// Cut rate (0.25-5, 1 = stock): cut chance x k; forehead meter x k.
// Bleed rate (0.25-5, 1 = stock): points per interval x k, delay / k, per-hit
// bleed x k. The interval stays stock.
// sub_82B3FA48 is no online check (DMG5 thought so): it returns 1 in offline fights.
//
// Doctor (sub_82B47BD0 is the gate, called after every cut roll that passes): it reads
// [0x8333B71C] (+0x166C; DMG5 had this one wrong as +0x1670). sub_82ED9FA0 sets it
// to 1 and only the modes in sub_82230488's cases 10, 13, 14 and 27 clear it. Past
// the gate, on an eye slot (1-4): with that eye at severity 2, a roll at [0x8333B724]
// / [0x8333B728] percent (static 0.5 / 0.75) brings the first visit. After it, cut
// rolls passed on the same eye side take the hit's damage from fighter+0xF90 (120);
// at 0 the fight stops.
// ufc3_doctor_stoppage = stock | on: on keeps the gate open and rolls at
// ufc3_doctor_chance percent.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "bare_knuckle.h"
#include "mod_pack.h"

REXCVAR_DEFINE_DOUBLE(ufc3_cut_rate, 1.5, "ufc3", "Gameplay, gloves: how easily cuts open, 1 = stock (0.25-5)");
REXCVAR_DEFINE_DOUBLE(ufc3_bleed_rate, 1.5, "ufc3", "Gameplay, gloves: how fast cuts bleed, 1 = stock (0.25-5)");
REXCVAR_DEFINE_DOUBLE(ufc3_bk_cut_rate, 3.0, "ufc3", "Gameplay, bare knuckle: how easily cuts open, 1 = stock (0.25-5)");
REXCVAR_DEFINE_DOUBLE(ufc3_bk_bleed_rate, 1.5, "ufc3", "Gameplay, bare knuckle: how fast cuts bleed, 1 = stock (0.25-5)");
REXCVAR_DEFINE_STRING(ufc3_doctor_stoppage, "stock", "ufc3",
                      "Gameplay: stock, or on (doctor checks stay allowed and roll at ufc3_doctor_chance percent)");
REXCVAR_DEFINE_DOUBLE(ufc3_doctor_chance, 5.0, "ufc3", "Doctor visit chance in percent when ufc3_doctor_stoppage = on");
REXCVAR_DEFINE_BOOL(ufc3_cut_force, false, "ufc3", "Debug: every head hit that can cut opens a cut");
REXCVAR_DEFINE_INT32(ufc3_doctor_left, 0, "ufc3", "Debug: after a doctor visit, cap the damage left until the stoppage (0 = stock)");
REXCVAR_DEFINE_BOOL(ufc3_cut_ramp, true, "ufc3", "Cut chance boost follows the strike's value: light strikes little, heavy ones more (off = flat)");
REXCVAR_DEFINE_BOOL(ufc3_cut_trace, false, "ufc3", "Log cuts, bleed points and doctor checks ([cut] lines; debug)");

namespace ufc3_cuts {

constexpr uint32_t kRulesHolder = 0x83576FD0;  // {UFC rule block, PRIDE rule block}
constexpr uint32_t kMatchFlags = 0x83541FE0;
constexpr uint32_t kDoctorGate = 0x8333B71C;
constexpr uint32_t kDoctorChanceA = 0x8333B724;
constexpr uint32_t kDoctorChanceB = 0x8333B728;
constexpr int kSlots = 11;

inline uint32_t Load32(uint8_t* base, uint32_t addr) { return ufc3_bk::Load32(base, addr); }
inline float LoadF(uint8_t* base, uint32_t addr) {
  uint32_t u = Load32(base, addr);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
inline void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = _byteswap_ulong(v);
  std::memcpy(base + addr, &v, 4);
}
inline void StoreF(uint8_t* base, uint32_t addr, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  Store32(base, addr, u);
}

inline double CutK() {
  if (!ufc3_pack::Active()) return 1.0;  // stock mode: stock cuts
  bool bare = ufc3_bk::Hands() == ufc3_bk::kBare;
  return std::clamp(bare ? REXCVAR_GET(ufc3_bk_cut_rate) : REXCVAR_GET(ufc3_cut_rate), 0.25, 5.0);
}
inline double BleedK() {
  if (!ufc3_pack::Active()) return 1.0;
  bool bare = ufc3_bk::Hands() == ufc3_bk::kBare;
  return std::clamp(bare ? REXCVAR_GET(ufc3_bk_bleed_rate) : REXCVAR_GET(ufc3_bleed_rate), 0.25, 5.0);
}
inline bool DoctorOn() { return ufc3_pack::Active() && REXCVAR_GET(ufc3_doctor_stoppage) == "on"; }

inline bool TraceBudget() {
  static std::atomic<int> n{0};
  int c = ++n;
  return c <= 1500 || c % 100 == 0;
}

// Rule block values and doctor settings, once per damage object.
inline void TraceRules(uint8_t* base, uint32_t dmg) {
  static uint32_t seen[4] = {};
  for (uint32_t s : seen)
    if (s == dmg) return;
  static int next = 0;
  seen[next++ & 3] = dmg;
  uint8_t kind = base[kMatchFlags + 0x32922];
  uint32_t holder = Load32(base, kRulesHolder);
  uint32_t r = holder ? Load32(base, holder + (kind == 1 || kind == 2 ? 4 : 0)) : 0;
  if (r) {
    std::string a;
    for (uint32_t off = 0x460; off <= 0x47C; off += 4) a += " " + std::to_string(LoadF(base, r + off)).substr(0, 7);
    REXLOG_INFO("[cut] rules {:08X} kind {} R460..47C{} R480 {} R484 {} R498 {} R4FC {} R500 {} R514 {} R518 {}", r, kind, a,
                Load32(base, r + 0x480), Load32(base, r + 0x484), Load32(base, r + 0x498), Load32(base, r + 0x4FC),
                Load32(base, r + 0x500), LoadF(base, r + 0x514), LoadF(base, r + 0x518));
  }
  REXLOG_INFO("[cut] doctor gate [8333B71C] = {}, [8333B720] = {}, chance {} / {}, cut k {:.2f}, bleed k {:.2f}, hands {}",
              Load32(base, kDoctorGate), Load32(base, kDoctorGate + 4), LoadF(base, kDoctorChanceA), LoadF(base, kDoctorChanceB),
              CutK(), BleedK(), ufc3_bk::Hands());
}

// One line whenever a fighter's cuts, bleed points or doctor state changed.
struct Seen {
  uint32_t dmg = 0, state[kSlots + 3] = {};
};
inline void TraceChange(uint8_t* base, uint32_t dmg, int zone) {
  static Seen seen[4];
  static int next = 0;
  Seen* s = nullptr;
  for (auto& e : seen)
    if (e.dmg == dmg) s = &e;
  if (!s) {
    s = &seen[next++ & 3];
    *s = Seen{};
    s->dmg = dmg;
  }
  uint32_t fighter = Load32(base, dmg);
  Seen now;
  std::string sev, pts;
  for (int i = 0; i < kSlots; ++i) {
    uint32_t v = Load32(base, dmg + 0x60 + 4 * i), p = Load32(base, dmg + 0x110 + 4 * i);
    now.state[i] = v * 100000 + p;
    sev += " " + std::to_string(v);
    pts += " " + std::to_string(static_cast<int32_t>(p));
  }
  now.state[kSlots] = Load32(base, fighter + 0xF80);      // doctor visits
  now.state[kSlots + 1] = Load32(base, fighter + 0xF90);  // what's left after a visit
  now.state[kSlots + 2] = Load32(base, dmg + 0x1C8);      // peak
  if (!std::memcmp(now.state, s->state, sizeof(now.state))) return;
  if (now.state[kSlots] != s->state[kSlots])
    REXLOG_INFO("[cut] DOCTOR VISIT fighter {:08X}: visits {} eye side {} left {}", fighter, now.state[kSlots],
                Load32(base, fighter + 0xF84), static_cast<int32_t>(now.state[kSlots + 1]));
  std::memcpy(s->state, now.state, sizeof(now.state));
  if (TraceBudget())
    REXLOG_INFO("[cut] dmg {:08X} zone {} severity{} points{} peak {} doctor {}/{}", dmg, zone, sev, pts,
                static_cast<int32_t>(now.state[kSlots + 2]), now.state[kSlots], static_cast<int32_t>(now.state[kSlots + 1]));
}

}  // namespace ufc3_cuts

REX_EXTERN(__imp__sub_82C73028);
REX_EXTERN(__imp__sub_82B47BD0);

// OnHit(dmg, attacker, hit, zone): trace.
extern "C" REX_FUNC(sub_82C73028) {
  namespace cu = ufc3_cuts;
  uint32_t dmg = ctx.r3.u32;
  int zone = static_cast<int>(ctx.r6.s32);
  uint32_t fighter = cu::Load32(base, dmg);
  bool trace = REXCVAR_GET(ufc3_cut_trace);
  if (trace && fighter) cu::TraceRules(base, dmg);
  int left = ufc3_pack::Active() ? REXCVAR_GET(ufc3_doctor_left) : 0;
  if (left > 0 && fighter && static_cast<int32_t>(cu::Load32(base, fighter + 0xF80)) >= 1 &&
      static_cast<int32_t>(cu::Load32(base, fighter + 0xF90)) > left)
    cu::Store32(base, fighter + 0xF90, left);
  __imp__sub_82C73028(ctx, base);
  if (trace && fighter) cu::TraceChange(base, dmg, zone);
}

// The doctor gate: open it for "on", count the checks for the trace.
extern "C" REX_FUNC(sub_82B47BD0) {
  namespace cu = ufc3_cuts;
  if (cu::DoctorOn()) {
    float chance = std::clamp(static_cast<float>(REXCVAR_GET(ufc3_doctor_chance)), 0.0f, 100.0f);
    cu::Store32(base, cu::kDoctorGate, 1);
    cu::StoreF(base, cu::kDoctorChanceA, chance);
    cu::StoreF(base, cu::kDoctorChanceB, chance);
  }
  uint32_t gate = cu::Load32(base, cu::kDoctorGate);
  __imp__sub_82B47BD0(ctx, base);
  if (REXCVAR_GET(ufc3_cut_trace)) {
    static std::atomic<int> n{0}, open{0};
    int o = ctx.r3.u32 ? ++open : open.load();
    REXLOG_INFO("[cut] doctor check #{}: gate {} display {:X} -> {} ({} open so far)", ++n, gate,
                cu::Load32(base, cu::kMatchFlags + 0x32980) & 0x30, ctx.r3.u32, o);
  }
}

// 0x82C72ECC: f0 = the first-visit roll (0-100), f13 = the chance it must not pass.
void ufc3_doctor_roll(PPCRegister& f0, PPCRegister& f13, PPCRegister& r24) {
  if (f0.f64 <= f13.f64) ufc3_blood::g_paint_pause_until = static_cast<int64_t>(GetTickCount64()) + 25000;  // the visit's cutscene
  if (REXCVAR_GET(ufc3_cut_trace))
    REXLOG_INFO("[cut] doctor roll slot={} {:.2f} vs {:.2f} -> {}", r24.s32, f0.f64, f13.f64, f0.f64 > f13.f64 ? "no" : "VISIT");
}

// 0x82C72F6C: the damage left after a visit ran out, r11 = fighter.
void ufc3_doctor_stop(PPCRegister& r11) {
  if (REXCVAR_GET(ufc3_cut_trace)) REXLOG_INFO("[cut] DOCTOR STOPPAGE fighter {:08X}", r11.u32);
}

// 0x82C70658, inside sub_82C70588(dmg, zone, class, value): r4 = the attack's class, r5 = the integer value the game hands
// to the slot's chance function, r21 = the caller's hit (sub_82C72858's third argument, untouched by this function).
inline thread_local uint32_t g_roll_class = 0, g_roll_value = 0, g_roll_hit = 0;
void ufc3_cut_hit(PPCRegister& r4, PPCRegister& r5, PPCRegister& r21) {
  g_roll_class = r4.u32;
  g_roll_value = r5.u32;
  g_roll_hit = r21.u32;
}

// 0x82C70660: r3 = cut chance in percent, just returned by the slot's function.
void ufc3_cut_chance(PPCRegister& r3) {
  namespace cu = ufc3_cuts;
  int in = r3.s32;
  if (in <= 0) return;  // this attack can't cut
  double k = cu::CutK();
  bool force = REXCVAR_GET(ufc3_cut_force) && ufc3_pack::Active();
  if (k == 1.0 && !force && !REXCVAR_GET(ufc3_cut_trace)) return;
  // The game's chance is already value x a rate (about 1.2 or 0.1): a flat k makes every strike from about 28 a sure cut.
  // Ramp: value <= 12 gets 1 + (k-1)/8, value >= 40 gets 1 + (k-1)*1.25 (bare k 3: 1.25 ... 3.5). User: jabs rarely cut, heavy shots do.
  double t = 1.0;
  if (REXCVAR_GET(ufc3_cut_ramp)) t = 0.125 + 1.125 * std::clamp((static_cast<int32_t>(g_roll_value) - 12) / 28.0, 0.0, 1.0);
  k = 1.0 + (k - 1.0) * t;
  int out = force ? 100 : std::min(100, static_cast<int>(in * k + 0.5));
  if (REXCVAR_GET(ufc3_cut_trace) && cu::TraceBudget()) {
    uint8_t* mem = REX_KERNEL_MEMORY()->virtual_membase();
    uint32_t hit = g_roll_hit;
    bool ok = hit >= 0x10000 && hit < 0xE0000000u;  // a guest pointer: never fault in a trace
    REXLOG_INFO("[cut] chance {} -> {} (k {:.2f}) class {} value {} hit {:08X} +60 {:.2f} [{:08X}]", in, out, k, g_roll_class,
                static_cast<int32_t>(g_roll_value), hit, ok ? cu::LoadF(mem, hit + 0x60) : 0.0f, ok ? cu::Load32(mem, hit + 0x60) : 0u);
  }
  r3.s64 = out;
}

// 0x82C73088: f0 = meter gain factor, r30 = zone. The forehead has no roll, only this.
void ufc3_cut_meter(PPCRegister& f0, PPCRegister& r30) {
  if (r30.s32 != 0) return;
  double k = ufc3_cuts::CutK();
  if (k != 1.0) f0.f64 = static_cast<float>(f0.f64 * k);
}

// 0x82C7237C: r3 = bleed points per interval.
void ufc3_bleed_step(PPCRegister& r3) {
  double k = ufc3_cuts::BleedK();
  if (k != 1.0) r3.s64 = static_cast<int>(r3.s32 * k + 0.5);
}

// 0x82C6F748: r3 = frames until a new cut starts bleeding, r30 = slot.
void ufc3_bleed_delay(PPCRegister& r3, PPCRegister& r30) {
  double k = ufc3_cuts::BleedK();
  if (REXCVAR_GET(ufc3_cut_trace)) REXLOG_INFO("[cut] open slot={} delay {} (k {:.2f})", r30.s32, r3.s32, k);
  if (k != 1.0) r3.s64 = static_cast<int>(r3.s32 / k);
}

// 0x82C72080: f0 = bleed points per damage on a hit to an open cut.
void ufc3_bleed_hit(PPCRegister& f0) {
  double k = ufc3_cuts::BleedK();
  if (k != 1.0) f0.f64 = static_cast<float>(f0.f64 * k);
}
