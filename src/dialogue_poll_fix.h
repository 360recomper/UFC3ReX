// ufc3 - the intro directors never miss a dialogue line (--ufc3_dialogue_no_skip).
//
// Bruce Buffer's ring intro (and the PRIDE ring announcer's) is a list of
// dialogue events (DAT/sound/ImtDialogEvent.dat) that the game resolves
// (sub_82F18A78) and queues on a Wwise dynamic sequence (sub_82F1A4D8). The
// intro directors move on only when they see certain lines playing: the UFC
// one (sub_82BAC5D8) through sub_827D6E68, which passes an item on only after
// it stayed current for 5 ticks, the PRIDE one through sub_82C36368 ("is line
// X playing on sequence obj or obj+28"). Both read sub_82F1AE28.
// Two cases make a line invisible and the intro then waits for Start:
//   - it resolves to a short filler node (a fighter without a recording of his
//     own, e.g. a fighter's UFC name line, node 35C0B4CE) that ends
//     within a frame or two;
//   - it resolves to nothing (node 0), so it is never queued (some added
//     fighters' PRIDE name line 11BF2047).
//
// With the option on, every line the game queues or fails to resolve is
// remembered per sequence. sub_82F1AE28 reports the line really playing, and
// slips in each one that was never seen (ended between two polls, flushed
// or never played) for kHold, in order. sub_827D6E68 then passes the value
// straight on instead of waiting 5 ticks, so lines are seen when they play,
// as in a stock intro (the next stage starts within ~2 frames of Buffer's
// last item EB206619; a later report is too late). Every caller sees the same
// value.

#pragma once

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(ufc3_dialogue_no_skip, false, "ufc3",
                    "Let the ring-intro directors see every dialogue line, including ones too short to "
                    "notice or without a recording");
// The narrow version: only the PRIDE ring announcer's lines that resolve to
// nothing (sub_82C36530 callers) are reported, in their turn; nothing else
// changes. Some added fighters' PRIDE name line 11BF2047 has no leaf for
// their key in the bank the game loads (the DLC packs' copy of
// events_structures.pck wins over the mods overlay), and the PRIDE intro waits
// for it to play.
REXCVAR_DEFINE_BOOL(ufc3_pride_dead_lines, false, "ufc3",
                    "Report PRIDE announcer lines without a recording as played, so the PRIDE intro goes on");

namespace ufc3_dialogue_poll {

using Clock = std::chrono::steady_clock;
// Two frames at 30 fps: every per-frame reader sees a missed line at least
// once. (sub_827D6E68's own 5-tick debounce is bypassed, see dialogue_trace.h.)
constexpr auto kHold = std::chrono::milliseconds(80);
// Between two lists the sequence reports 0 for a moment before the next line
// starts; only a longer 0 means everything left was flushed or never plays.
constexpr auto kIdle = std::chrono::milliseconds(200);

struct Line {
  uint32_t event;
  bool dead;  // resolved to nothing: never plays
};

struct Seq {
  std::deque<Line> queued;       // in queue order, not yet seen playing
  std::deque<uint32_t> pending;  // ended unseen, still to report
  Clock::time_point since{};     // when pending.front() was first reported
  bool holding = false;
  uint32_t real = 0;             // what Wwise reported last
  Clock::time_point zero_since{};  // when real became 0
  Clock::time_point last_queued{};  // when a line was last queued
};

inline std::mutex& Mutex() {
  static std::mutex m;
  return m;
}

inline std::unordered_map<uint32_t, Seq>& Seqs() {
  static std::unordered_map<uint32_t, Seq> m;
  return m;
}

inline void Queued(uint32_t seq, uint32_t event, bool dead) {
  std::lock_guard lock(Mutex());
  Seq& s = Seqs()[seq];
  s.queued.push_back({event, dead});
  s.last_queued = Clock::now();
}

// `real` = line playing now (0 = none); returns the line to report. With
// `dead_only`, lines that played (seen or not) are dropped and only the ones
// that never play are reported; sequences with nothing recorded are left alone.
inline uint32_t Poll(uint32_t seq, uint32_t real, bool dead_only = false) {
  std::lock_guard lock(Mutex());
  auto found = Seqs().find(seq);
  if (found == Seqs().end()) {
    if (dead_only) return real;
    found = Seqs().try_emplace(seq).first;
  }
  Seq& s = found->second;
  size_t before = s.pending.size();
  auto now = Clock::now();
  if (real != s.real) {
    if (real != 0) {
      // Everything queued ahead of the new line is over, seen or not.
      auto it = s.queued.begin();
      while (it != s.queued.end() && !(it->event == real && !it->dead)) ++it;
      if (it != s.queued.end()) {
        for (auto j = s.queued.begin(); j != it; ++j)
          if (j->dead || (!dead_only && j->event != s.real)) s.pending.push_back(j->event);
        s.queued.erase(s.queued.begin(), it + 1);
      }
    } else {
      s.zero_since = now;
    }
    s.real = real;
  } else if (real == 0) {
    // Idle: lines that never play come up at once; after kIdle, so does
    // anything left in the queue (flushed).
    bool idle = now - std::max(s.zero_since, s.last_queued) >= kIdle;
    while (!s.queued.empty() && (s.queued.front().dead || idle)) {
      if (s.queued.front().dead || !dead_only) s.pending.push_back(s.queued.front().event);
      s.queued.pop_front();
    }
  }
  if (s.pending.size() > before)
    REXLOG_INFO("[dlg-fix] seq {:08X}: {} line(s) ended unseen, reporting them", seq,
                s.pending.size() - before);
  if (s.pending.empty()) return real;
  if (!s.holding) {
    s.holding = true;
    s.since = now;
  } else if (now - s.since >= kHold) {
    s.pending.pop_front();
    s.since = now;
    if (s.pending.empty()) {
      s.holding = false;
      return real;
    }
  }
  return s.pending.front();
}

}  // namespace ufc3_dialogue_poll
