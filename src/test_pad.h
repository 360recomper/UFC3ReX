// ufc3 - test aid: a pad-0 press list that cannot drop presses (off by default).
//
// --ufc3_test_pad_script=<file>. One step per line, '#' starts a comment:
//   <wait s> <BUTTON...>        wait <s> after the previous step ended, then press the buttons
//   <wait s> mark <text>        log "[testpad] mark <text>" (for ufc_selftest.py --on-log captures)
// Buttons: A B X Y START BACK LB RB UP DOWN LEFT RIGHT. "hold=<s>" on a press line holds it longer (menus repeat).
// The input_script pads (sdk_patches 0029) run on wall time, so a press could fall inside one long frame (menu
// picture loads) and never be seen: here a press stays down until the game has polled pad 0 at least twice AND
// the hold time (default 0.05 s, short so menus do not auto-repeat) has passed, then stays up for 4 polls
// before the next wait starts. Needs pad 0 connected (an
// input_script with "pad 0", or a real pad). Presses are ORed on top of pad 0 through bare_knuckle.h's pad hook.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "bare_knuckle.h"

REXCVAR_DEFINE_STRING(ufc3_test_pad_script, "", "ufc3",
                      "Test aid: pad-0 presses from a file, each held until the game has seen it (see src/test_pad.h)");

namespace ufc3_test_pad {

struct Step {
  double wait = 0, hold = 0.05;
  uint16_t buttons = 0;
  std::string mark;
};

struct State {
  bool loaded = false;
  std::vector<Step> steps;
  size_t next = 0;
  int phase = 0;  // 0 waiting, 1 down, 2 up
  int polls = 0;
  uint32_t bump = 0;
  std::chrono::steady_clock::time_point since;
};
inline State g;

inline double Secs(std::chrono::steady_clock::time_point a) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
}

inline uint16_t Button(const std::string& w) {
  static const std::pair<const char*, uint16_t> k[] = {
      {"UP", 0x0001}, {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008}, {"START", 0x0010}, {"BACK", 0x0020},
      {"LB", 0x0100}, {"RB", 0x0200}, {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000}};
  for (auto& [n, b] : k)
    if (w == n) return b;
  return 0;
}

inline void Load() {
  g.loaded = true;
  std::string path = REXCVAR_GET(ufc3_test_pad_script);
  if (path.empty()) return;
  std::ifstream in(path);
  if (!in) {
    REXLOG_ERROR("[testpad] cannot open {}", path);
    return;
  }
  std::string line;
  while (std::getline(in, line)) {
    line = line.substr(0, line.find('#'));
    std::istringstream ws(line);
    std::string first;
    if (!(ws >> first)) continue;
    Step s;
    s.wait = std::atof(first.c_str());
    std::string w;
    while (ws >> w) {
      if (w == "mark") {
        std::getline(ws, s.mark);
        s.mark = s.mark.empty() ? "-" : s.mark.substr(1);
        break;
      }
      if (w.rfind("hold=", 0) == 0) {
        s.hold = std::atof(w.c_str() + 5);
        continue;
      }
      std::transform(w.begin(), w.end(), w.begin(), [](unsigned char c) { return char(std::toupper(c)); });
      if (uint16_t b = Button(w)) s.buttons |= b;
      else REXLOG_ERROR("[testpad] unknown button '{}'", w);
    }
    g.steps.push_back(s);
  }
  g.since = std::chrono::steady_clock::now();
  REXLOG_INFO("[testpad] {} steps from {}", g.steps.size(), path);
}

inline uint16_t Tick() {
  if (!g.loaded) Load();
  while (g.next < g.steps.size()) {
    Step& s = g.steps[g.next];
    if (g.phase == 0) {
      if (Secs(g.since) < s.wait) return 0;
      if (!s.mark.empty()) {
        REXLOG_INFO("[testpad] mark {}", s.mark);
        ++g.next;
        g.since = std::chrono::steady_clock::now();
        continue;
      }
      g.phase = 1;
      g.polls = 0;
      g.since = std::chrono::steady_clock::now();
      ++g.bump;
    }
    if (g.phase == 1) {
      if (++g.polls < 2 || Secs(g.since) < s.hold) return s.buttons;
      g.phase = 2;
      g.polls = 0;
      ++g.bump;
      return 0;
    }
    if (++g.polls < 4 || Secs(g.since) < 0.1) return 0;
    g.phase = 0;
    ++g.next;
    g.since = std::chrono::steady_clock::now();
    if (g.next == g.steps.size()) REXLOG_INFO("[testpad] done");
    return 0;
  }
  return 0;
}

inline void (*g_prev_pad)(uint8_t*, uint32_t, uint32_t) = nullptr;
inline void OnPad(uint8_t* base, uint32_t user, uint32_t state) {
  if (g_prev_pad) g_prev_pad(base, user, state);
  if (user != 0 || REXCVAR_GET(ufc3_test_pad_script).empty()) return;
  uint16_t add = Tick();
  uint16_t b = uint16_t(base[state + 4] << 8 | base[state + 5]) | add;
  base[state + 4] = uint8_t(b >> 8);
  base[state + 5] = uint8_t(b);
  uint32_t pkt = uint32_t(base[state] << 24 | base[state + 1] << 16 | base[state + 2] << 8 | base[state + 3]) + g.bump;
  base[state] = uint8_t(pkt >> 24);
  base[state + 1] = uint8_t(pkt >> 16);
  base[state + 2] = uint8_t(pkt >> 8);
  base[state + 3] = uint8_t(pkt);
}
inline const bool kPadHook = (g_prev_pad = ufc3_bk::g_on_pad, ufc3_bk::g_on_pad = &OnPad, true);

}  // namespace ufc3_test_pad
