// ufc3 - per-fighter walkout music (--ufc3_fighter_music).
//
// Audio is Wwise 2011 (bank version 62): DAT/sound/sound.pck holds the banks
// and the streamed XMA music. Every sound goes through
// AK::SoundEngine::PostEvent (sub_82F19390: r3 event, r4 game object, r5
// callback flags), called only by the game's sound manager (0x827D68E0 ff.).
//
// Walkouts (Wwise names recovered from their FNV-1 hashes):
//   sub_82C35938(corner) prepares a corner's walkout: it reads the fighter
//   record at 0x83541FE0 + corner * 20712 (corner 0 red, 1 blue; +0 = fighter
//   id * 100) and sets the Wwise state group "fighter_entrance_music"
//   (0x2C5E9999) to that fighter's track (record +160, or +172 for some id
//   ranges; created fighters use their CAF choice, default "caf_random_music").
//   The walkout then posts "play_fighter_entrance_red" (0xDA3E260E) or
//   "play_fighter_entrance_blue" (0x6891CBA5): a music switch container on
//   that state group (297 states, the licensed soundtrack). So the game already
//   picks music per fighter; it is just limited to the disc's tracks.
//
// With the option on and a track mapped for the fighter, we set the state to
// "none" (no music node: the container plays nothing, which the game already
// does for many states) right before the play event, post the event as usual
// so the walkout's timing and callbacks are untouched, and stream the user's
// file on the host (music_player.h). The track stops when Wwise reports the
// end of that playing ID to the game's event callback (sub_827D6740), i.e.
// whenever the game stops its own walkout music, and pauses with the game's
// "pause_all" / "resume_all" events. Fighters without a mapping keep the
// vanilla track.

#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "music_player.h"

REXCVAR_DEFINE_BOOL(ufc3_fighter_music, false, "ufc3",
                    "Play your own walkout music per fighter (music/fighter_music.toml)");
REXCVAR_DEFINE_STRING(ufc3_fighter_music_dir, "", "ufc3",
                      "Folder with fighter_music.toml and the tracks (default: <mods_root>/music, "
                      "or music next to the exe)");
REXCVAR_DEFINE_BOOL(ufc3_trace_sound, false, "ufc3",
                    "Log every Wwise PostEvent (event id, game object, caller chain)");

REX_EXTERN(__imp__sub_82F19390);  // AK::SoundEngine::PostEvent(id, obj, flags, cb, cookie, ...)
REX_EXTERN(__imp__sub_827D6740);  // game's Wwise event callback(type, info)
REX_EXTERN(sub_82F16520);         // AK::SoundEngine::SetState(group, state)

namespace ufc3_fighter_music {

constexpr uint32_t kEntranceRed = 0xDA3E260E;     // play_fighter_entrance_red
constexpr uint32_t kEntranceBlue = 0x6891CBA5;    // play_fighter_entrance_blue
// Two more events play the red / blue entrance containers (from the heckle
// banks); treat them the same way.
constexpr uint32_t kEntranceRedAlt = 0x27F49BFD;
constexpr uint32_t kEntranceBlueAlt = 0x7A34B1DE;
constexpr uint32_t kPauseAll = 0xE6517101;        // pause_all
constexpr uint32_t kResumeAll = 0xDB54B788;       // resume_all
constexpr uint32_t kEntranceGroup = 0x2C5E9999;   // state group fighter_entrance_music
constexpr uint32_t kStateNone = 0;                // SetState maps "none" (0x2CA33BDB) to 0
constexpr uint32_t kCornerBase = 0x83541FE0;      // fight setup, one record per corner
constexpr uint32_t kCornerStride = 20712;
constexpr uint32_t kAkEndOfEvent = 1;

using ufc3_music::MusicPlayer;

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}

struct Config {
  std::filesystem::path dir;
  std::filesystem::path file;
  std::filesystem::file_time_type mtime{};
  bool loaded = false;
  float volume = 1.0f;
  float fade_out = 1.0f;
  bool loop = true;
  std::unordered_map<uint32_t, std::string> tracks;  // fighter id -> file
  std::string any;                                   // "*": every unmapped fighter
};

inline std::filesystem::path MusicDir() {
  std::string dir = REXCVAR_GET(ufc3_fighter_music_dir);
  if (!dir.empty()) return std::filesystem::path(dir);
  std::string mods = rex::cvar::GetFlagByName("mods_root");
  if (!mods.empty()) return std::filesystem::path(mods) / "music";
  return rex::filesystem::GetExecutableFolder() / "music";
}

// (Re)reads fighter_music.toml when it changed, so edits apply on the next
// walkout without restarting.
inline void Refresh(Config& cfg) {
  cfg.dir = MusicDir();
  cfg.file = cfg.dir / "fighter_music.toml";
  std::error_code ec;
  auto mtime = std::filesystem::last_write_time(cfg.file, ec);
  if (ec) {
    if (cfg.loaded || cfg.tracks.size()) {
      REXLOG_WARN("[fighter_music] {} not found", cfg.file.string());
    }
    cfg.loaded = false;
    cfg.tracks.clear();
    cfg.any.clear();
    return;
  }
  if (cfg.loaded && mtime == cfg.mtime) return;
  cfg.loaded = true;
  cfg.mtime = mtime;
  cfg.tracks.clear();
  cfg.any.clear();
  toml::table tbl;
#if TOML_EXCEPTIONS
  try {
    tbl = toml::parse_file(cfg.file.string());
  } catch (const toml::parse_error& e) {
    REXLOG_WARN("[fighter_music] {}: {}", cfg.file.string(), e.description());
    return;
  }
#else
  auto result = toml::parse_file(cfg.file.string());
  if (!result) {
    REXLOG_WARN("[fighter_music] {}: {}", cfg.file.string(), result.error().description());
    return;
  }
  tbl = std::move(result).table();
#endif
  cfg.volume = static_cast<float>(tbl["volume"].value_or(1.0));
  cfg.volume = std::clamp(cfg.volume, 0.0f, 4.0f);
  cfg.fade_out = std::clamp(static_cast<float>(tbl["fade_out"].value_or(1.0)), 0.0f, 10.0f);
  cfg.loop = tbl["loop"].value_or(true);
  if (auto* fighters = tbl["fighters"].as_table()) {
    for (auto&& [key, val] : *fighters) {
      auto file = val.value<std::string>();
      if (!file) continue;
      std::string k(key.str());
      if (k == "*") {
        cfg.any = *file;
        continue;
      }
      char* end = nullptr;
      unsigned long id = std::strtoul(k.c_str(), &end, 10);
      if (end == k.c_str() || *end) {
        REXLOG_WARN("[fighter_music] ignoring key \"{}\" (use the fighter id from the log)", k);
        continue;
      }
      cfg.tracks[static_cast<uint32_t>(id)] = *file;
    }
  }
  // [[track]] file = "x.mp3", fighters = [22, 309, 1005]: one file for every
  // version of a fighter (PRIDE, alternate-mode ids).
  if (auto* list = tbl["track"].as_array()) {
    for (auto&& node : *list) {
      auto* t = node.as_table();
      if (!t) continue;
      auto file = (*t)["file"].value<std::string>();
      auto* ids = (*t)["fighters"].as_array();
      if (!file || !ids) {
        REXLOG_WARN("[fighter_music] [[track]] needs file = \"...\" and fighters = [ids]");
        continue;
      }
      for (auto&& id : *ids) {
        if (auto v = id.value<int64_t>(); v && *v >= 0) {
          cfg.tracks[static_cast<uint32_t>(*v)] = *file;
        }
      }
    }
  }
  REXLOG_INFO("[fighter_music] loaded {}: {} fighter(s){}, volume {}, loop {}",
              cfg.file.string(), cfg.tracks.size(), cfg.any.empty() ? "" : " + default",
              cfg.volume, cfg.loop);
}

struct State {
  std::mutex mutex;
  Config cfg;
  std::atomic<uint32_t> playing_id{0};  // Wwise playing id our track follows
  std::atomic<float> fade_out{1.0f};
};

inline State& Get() {
  static State state;
  return state;
}

// Picks the file for `fighter`, or nothing (vanilla music).
inline std::optional<std::filesystem::path> TrackFor(Config& cfg, uint32_t fighter) {
  auto it = cfg.tracks.find(fighter);
  const std::string& name = it != cfg.tracks.end() ? it->second : cfg.any;
  if (name.empty()) return std::nullopt;
  std::filesystem::path path = std::filesystem::path(name);
  if (path.is_relative()) path = cfg.dir / path;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    REXLOG_WARN("[fighter_music] fighter {}: {} not found, using the game's track", fighter,
                path.string());
    return std::nullopt;
  }
  return path;
}

inline void TraceEvent(PPCContext& ctx, uint8_t* base, uint32_t id, uint32_t obj, uint32_t flags,
                       uint32_t lr, uint32_t result) {
  // Saved LRs up the guest stack: each frame's back chain at [sp], the
  // caller's LR at [back chain - 8].
  uint32_t chain[4] = {};
  uint32_t sp = ctx.r1.u32;
  for (auto& c : chain) {
    uint32_t back = Load32(base, sp);
    if (back <= sp || back - sp > 0x100000) break;
    c = Load32(base, back - 8);
    sp = back;
  }
  REXLOG_INFO("[snd] PostEvent {:08X} obj={:X} flags={:X} -> {:X} lr={:08X} {:08X} {:08X} {:08X} {:08X}",
              id, obj, flags, result, lr, chain[0], chain[1], chain[2], chain[3]);
}

}  // namespace ufc3_fighter_music

extern "C" REX_FUNC(sub_82F19390) {
  namespace fm = ufc3_fighter_music;
  const uint32_t id = ctx.r3.u32, obj = ctx.r4.u32, flags = ctx.r5.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t sp = ctx.r1.u32;

  int corner = -1;
  if (REXCVAR_GET(ufc3_fighter_music)) {
    if (id == fm::kEntranceRed || id == fm::kEntranceRedAlt) corner = 0;
    if (id == fm::kEntranceBlue || id == fm::kEntranceBlueAlt) corner = 1;
    if (id == fm::kPauseAll) fm::MusicPlayer::Get().Pause(true);
    if (id == fm::kResumeAll) fm::MusicPlayer::Get().Pause(false);
  }

  std::optional<std::filesystem::path> track;
  fm::State& st = fm::Get();
  std::unique_lock lock(st.mutex, std::defer_lock);
  if (corner >= 0) {
    lock.lock();
    fm::Refresh(st.cfg);
    st.fade_out.store(st.cfg.fade_out);
    uint32_t raw = fm::Load32(base, fm::kCornerBase + corner * fm::kCornerStride);
    uint32_t fighter = raw / 100;
    track = fm::TrackFor(st.cfg, fighter);
    REXLOG_INFO("[fighter_music] {} corner walkout: fighter {} (raw {}): {}",
                corner ? "blue" : "red", fighter, raw,
                track ? track->filename().string() : std::string("game track"));
    // A new walkout always ends the previous one's music.
    st.playing_id.store(0);
    fm::MusicPlayer::Get().Stop(st.cfg.fade_out);
    if (track) {
      // Mute the game's track: "none" has no music node in the container.
      PPCContext c = ctx;
      c.r1.u64 = (sp - 0x200) & ~0xFull;
      c.r3.u64 = fm::kEntranceGroup;
      c.r4.u64 = fm::kStateNone;
      sub_82F16520(c, base);
    }
    lock.unlock();
  }

  __imp__sub_82F19390(ctx, base);

  if (track) {
    st.playing_id.store(ctx.r3.u32);
    std::lock_guard relock(st.mutex);
    fm::MusicPlayer::Get().Play(*track, st.cfg.volume, st.cfg.loop);
  }
  if (REXCVAR_GET(ufc3_trace_sound)) {
    PPCContext t = ctx;
    t.r1.u64 = sp;
    fm::TraceEvent(t, base, id, obj, flags, lr, ctx.r3.u32);
  }
}

extern "C" REX_FUNC(sub_827D6740) {
  namespace fm = ufc3_fighter_music;
  if (ctx.r3.u32 == fm::kAkEndOfEvent && ctx.r4.u32) {
    uint32_t playing = fm::Load32(base, ctx.r4.u32 + 8);
    uint32_t ours = fm::Get().playing_id.load();
    if (ours && playing == ours) {
      fm::Get().playing_id.store(0);
      REXLOG_INFO("[fighter_music] game ended its walkout music; stopping track");
      fm::MusicPlayer::Get().Stop(fm::Get().fade_out.load());
    }
  }
  __imp__sub_827D6740(ctx, base);
}
