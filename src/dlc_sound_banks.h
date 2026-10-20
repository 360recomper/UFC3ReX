// ufc3 - DLC bouts use the game's own (mod-overlaid) dialogue banks
// (--ufc3_dlc_banks_from_game, on by default).
//
// When a DLC fighter (IDs 1300-1379) is in the bout, sub_827D42E0 points the
// Wwise low-level IO at the DLC package's folder and loads its
// events_structures.pck and dlc_media.pck. The DLC copy of
// events_structures.pck is byte-identical to the disc's, but the package
// loaded last wins, so every bank loaded from then on (the PRIDE intro's
// 144611B9 / 73C2A355 among them) comes from the DLC copy and a mod's edited
// dialogue banks (mods overlay of DAT/sound/events_structures.pck) are
// ignored. The DLC files can't be patched: the game verifies them on mount.
//
// With the option on, that one load (the call returning to 0x827D4484) is
// made from game:\dat\sound\ instead, where the mods overlay applies; the base
// path is then set back to the DLC folder for dlc_media.pck.

#pragma once

#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(ufc3_dlc_banks_from_game, true, "ufc3",
                    "In bouts with a DLC fighter, load events_structures.pck from the game (mods) "
                    "folder instead of the DLC copy");

REX_EXTERN(__imp__sub_827D6008);  // LoadFilePackage(io, name, out id) -> 0 = ok
REX_EXTERN(sub_827D9910);         // low-level IO SetBasePath(path)

namespace ufc3_dlc_banks {

constexpr uint32_t kDlcEventsLoad = 0x827D4484;  // return address in sub_827D42E0 (DLC packages)
constexpr uint32_t kGameSoundPath = 0x820F1EFC;  // "game:\dat\sound\" (sub_827D42E0's unload path)
constexpr uint32_t kDlcPathInCaller = 80;        // sub_827D42E0 builds the DLC folder at sp+80

inline void SetBasePath(PPCContext& ctx, uint8_t* base, uint32_t path) {
  PPCContext c = ctx;
  c.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFull;
  c.r3.u64 = path;
  sub_827D9910(c, base);
}

}  // namespace ufc3_dlc_banks

extern "C" REX_FUNC(sub_827D6008) {
  namespace db = ufc3_dlc_banks;
  const uint32_t name = ctx.r4.u32;
  const bool redirect = REXCVAR_GET(ufc3_dlc_banks_from_game) &&
                        static_cast<uint32_t>(ctx.lr) == db::kDlcEventsLoad && name &&
                        std::strcmp(reinterpret_cast<const char*>(base + name), "events_structures.pck") == 0;
  if (!redirect) {
    __imp__sub_827D6008(ctx, base);
    return;
  }
  const uint32_t dlc_path = ctx.r1.u32 + db::kDlcPathInCaller;
  PPCContext saved = ctx;
  db::SetBasePath(saved, base, db::kGameSoundPath);
  __imp__sub_827D6008(ctx, base);
  db::SetBasePath(saved, base, dlc_path);
  REXLOG_INFO("[dlc-banks] DLC bout: events_structures.pck loaded from game:\\dat\\sound\\ (mods overlay) "
              "instead of {} -> {}",
              reinterpret_cast<const char*>(base + dlc_path), ctx.r3.s32);
}
