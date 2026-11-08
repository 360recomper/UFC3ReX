// ufc3 - game speed holds when the frame rate dips (--ufc3_adaptive_step).
//
// The fight keeps the 60 fps values in the frame-rate block (cutscene_60.h), so a PC that renders
// 44 fps steps 1/60 s per frame and the game runs at 73% speed. Once per presented frame this
// writes the block's step values for the real frame time, between 60 and 30 fps. The rate field
// (+0x08) stays 60, so the present interval and the code that branches on the rate do not change.
// The block stays in the game's own form: fps * ticks and ms * fps stay constant, as in the
// game's 60/50/30 cases.
//
// Adapted from app/common/ufc_adaptive_rate.cpp in UFC3MONS
// (https://github.com/monscriser/UFC3MONS):
//
// MIT License
//
// Copyright (c) 2026 monscriser, UFC3MONS contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "native/ufc_native.h"

REXCVAR_DEFINE_STRING(ufc3_adaptive_step, "auto", "ufc3",
                      "Keep game speed when the frame rate dips: auto (native renderer only), on, off");
REXCVAR_DEFINE_DOUBLE(ufc3_adaptive_step_min_fps, 30.0, "ufc3",
                      "Lowest frame rate the step follows, 30..60; below it the game slows down");
REXCVAR_DEFINE_BOOL(ufc3_adaptive_step_log, false, "ufc3",
                    "Log frame rate and game step every 2 seconds ([adaptive-step])");
REXCVAR_DEFINE_DOUBLE(ufc3_adaptive_step_test_fps, 0.0, "ufc3",
                      "Test only: cap the frame rate to imitate a slow PC, works with the step off too; 0 = no cap");

namespace ufc3_adaptive {

constexpr uint32_t kBlock = 0x833C80B8;
constexpr uint32_t kRate = kBlock + 0x08, kLock = kBlock + 0x0C, kFps = kBlock + 0x1C,
                   kMs = kBlock + 0x20, kMsInt = kBlock + 0x24, kTicks = kBlock + 0x38,
                   kTicksInv = kBlock + 0x3C;
constexpr uint32_t kPresentFlag = 176;  // sub_8273BBE0 presents only when r3+176 is set
constexpr double kFullMs = 1000.0 / 60.0;
constexpr double kKeepUpMs = 17.5;  // an average under this gets the game's exact 60 values

using Clock = std::chrono::steady_clock;

struct State {
  Clock::time_point last{};
  double smooth_ms = kFullMs;
  bool adjusted = false;  // the block holds our values
  Clock::time_point log_start{};
  int log_frames = 0, log_adjusted = 0;
  double log_step_ms = 0, log_lost_ms = 0, log_min = 1e9, log_max = 0;
};
inline State g;
inline std::mutex g_block_mutex;  // the game's setters may run on another thread (not checked)

inline bool Enabled() {
  const std::string& mode = REXCVAR_GET(ufc3_adaptive_step);
  return mode == "on" || (mode == "auto" && g_ufc3_native);
}

// Held around the game's own block setters while the feature is on.
inline std::unique_lock<std::mutex> LockForSetter() {
  if (!Enabled()) return {};
  return std::unique_lock<std::mutex>(g_block_mutex);
}

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}

inline void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = _byteswap_ulong(v);
  std::memcpy(base + addr, &v, 4);
}

inline float LoadF32(uint8_t* base, uint32_t addr) {
  const uint32_t bits = Load32(base, addr);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

inline void StoreF32(uint8_t* base, uint32_t addr, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  Store32(base, addr, bits);
}

inline void WriteBlock(uint8_t* base, double ms) {
  if (ms <= kFullMs) {  // the game's own 60 values, exactly
    StoreF32(base, kFps, 60.0f);
    StoreF32(base, kMs, 16.666666f);
    Store32(base, kMsInt, 16);
    StoreF32(base, kTicks, 1.0f);
    StoreF32(base, kTicksInv, 1.0f);
    return;
  }
  StoreF32(base, kFps, float(1000.0 / ms));
  StoreF32(base, kMs, float(ms));
  Store32(base, kMsInt, uint32_t(ms));
  StoreF32(base, kTicks, float(ms / kFullMs));
  StoreF32(base, kTicksInv, float(kFullMs / ms));
}

// Called at the top of sub_8273BBE0. scene_held: --ufc3_cutscene_60 kept a scene at 60 that the
// game wanted at 30; its timing is not understood yet, so it keeps the plain 60 values.
inline void Frame(uint8_t* base, uint32_t device, bool scene_held) {
  const bool on = Enabled();
  const double cap_fps = REXCVAR_GET(ufc3_adaptive_step_test_fps);
  const bool log = REXCVAR_GET(ufc3_adaptive_step_log);
  if (!on && cap_fps <= 0 && !log) return;
  if (Load32(base, device + kPresentFlag) == 0) return;  // this call will not present: not a frame

  auto now = Clock::now();
  if (cap_fps > 0 && g.last != Clock::time_point{}) {
    const auto until = g.last + std::chrono::duration_cast<Clock::duration>(
                                    std::chrono::duration<double>(1.0 / cap_fps));
    while (now < until) {
      std::this_thread::yield();
      now = Clock::now();
    }
  }
  const double frame_ms = g.last == Clock::time_point{}
                              ? kFullMs
                              : std::chrono::duration<double, std::milli>(now - g.last).count();
  g.last = now;
  const double ceiling_ms = 1000.0 / std::clamp(REXCVAR_GET(ufc3_adaptive_step_min_fps), 30.0, 60.0);

  uint32_t rate;
  {
    std::unique_lock<std::mutex> lock;
    if (on) lock = std::unique_lock<std::mutex>(g_block_mutex);
    rate = Load32(base, kRate);
    if (on && rate == 60 && Load32(base, kLock) == 0 && !scene_held) {
      // Short average: follows a drop within a few frames, ignores single-frame jitter.
      g.smooth_ms += (std::clamp(frame_ms, kFullMs, ceiling_ms) - g.smooth_ms) * 0.25;
      const double ms = g.smooth_ms < kKeepUpMs ? kFullMs : std::min(g.smooth_ms, ceiling_ms);
      if (ms > kFullMs || g.adjusted) {
        WriteBlock(base, ms);
        g.adjusted = ms > kFullMs;
      }
    } else {
      if (g.adjusted && rate == 60) WriteBlock(base, kFullMs);
      g.smooth_ms = kFullMs;
      g.adjusted = false;  // at 30/25 the game's own setter already rewrote the block
    }
  }

  if (!log) return;
  if (g.log_start == Clock::time_point{}) g.log_start = now;
  ++g.log_frames;
  g.log_adjusted += g.adjusted ? 1 : 0;
  g.log_step_ms += LoadF32(base, kMs);
  g.log_lost_ms += std::max(frame_ms - ceiling_ms, 0.0);
  g.log_min = std::min(g.log_min, frame_ms);
  g.log_max = std::max(g.log_max, frame_ms);
  const double real_ms = std::chrono::duration<double, std::milli>(now - g.log_start).count();
  if (real_ms < 2000) return;
  REXLOG_INFO(
      "[adaptive-step] fps={:.1f} on={} rate={} held={} ticks={:.3f} step_per_real={:.3f} "
      "frame_ms={:.1f}..{:.1f} over_ceiling_ms={:.0f} adjusted={}/{}",
      g.log_frames * 1000.0 / real_ms, on ? 1 : 0, rate, scene_held ? 1 : 0,
      LoadF32(base, kTicks), g.log_step_ms / real_ms, g.log_min, g.log_max, g.log_lost_ms,
      g.log_adjusted, g.log_frames);
  g.log_start = now;
  g.log_frames = g.log_adjusted = 0;
  g.log_step_ms = g.log_lost_ms = g.log_max = 0;
  g.log_min = 1e9;
}

}  // namespace ufc3_adaptive
