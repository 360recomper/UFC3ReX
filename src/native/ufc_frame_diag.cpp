// Frame hitch diagnostics (off unless svr_frame_diag_ms is set, e.g. 22): a frame slower than that
// is logged with the time it spent compiling pipelines, building textures and render targets,
// checking textures for changes, in draws, waiting for the GPU, the swap chain and (the game) for
// the emulated GPU. A slow frame with a long GPU wait is the graphics card's limit; one without
// is a stall on the CPU side. Every 10 s a summary line gives the slowest frame and how many
// were slow.

#include "ufc_frame_diag.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/cvar.h>

#include "core/logging.h"

REXCVAR_DEFINE_INT32(svr_frame_diag_ms, 0, "SvR",
                     "Log frames slower than this many ms with what they spent the time on "
                     "(0 = off)");
REXCVAR_DEFINE_BOOL(ufc3_native_perf, false, "ufc3",
                    "Native renderer: one [nperf] line a second (draw-heavy scenes only) with where "
                    "the frame thread's time went, ms per frame");
REXCVAR_DEFINE_INT32(ufc3_stall_trace_ms, 0, "ufc3",
                     "Native renderer: when no frame has been presented for this many ms, write "
                     "every thread's stack to native_stall_trace.txt, again every 100 ms, up to 10 "
                     "times a stall (0 = off)");

void SvrStallTraceStart(int ms);  // crash_trace.cpp

namespace bd::gpu {

namespace {

struct Slot {
  std::atomic<uint64_t> us{0};
  std::atomic<uint64_t> ticks{0};  // SvrDiagTimer's, converted in SvrDiagEndFrame
  std::atomic<uint32_t> n{0};
};
std::array<Slot, size_t(SvrDiag::kCount)> g_slots;

using Clock = std::chrono::steady_clock;

double Ms(uint64_t us) { return double(us) / 1000.0; }

}  // namespace

void SvrDiagAdd(SvrDiag what, uint64_t microseconds) {
  auto& slot = g_slots[size_t(what)];
  slot.us.fetch_add(microseconds, std::memory_order_relaxed);
  slot.n.fetch_add(1, std::memory_order_relaxed);
}

// The thread that presents (and calls SvrDiagEndFrame) is the one that draws, so its timers add
// to plain thread-local counters; any other thread uses the shared atomics.
struct LocalSlot {
  uint64_t ticks = 0;
  uint32_t n = 0;
};
thread_local bool t_frame_thread = false;
thread_local std::array<LocalSlot, size_t(SvrDiag::kCount)> t_local;

void SvrDiagAddTicks(SvrDiag what, uint64_t ticks) {
  if (t_frame_thread) {
    auto& l = t_local[size_t(what)];
    l.ticks += ticks;
    ++l.n;
    return;
  }
  auto& slot = g_slots[size_t(what)];
  slot.ticks.fetch_add(ticks, std::memory_order_relaxed);
  slot.n.fetch_add(1, std::memory_order_relaxed);
}

// ---- ufc3_native_perf -------------------------------------------------------------------------

std::atomic<bool> g_svr_perf_on{false};

namespace {

constexpr size_t kZones = size_t(SvrZoneId::kCount);
constexpr size_t kCounts = size_t(SvrCount::kCount);
// Same order as SvrZoneId.
constexpr const char *kZoneNames[kZones] = {
    "game", "gd3d", "gwait", "gsleep", "state", "tex", "geom",  "ghash", "unlock", "up",
    "draw", "rs",   "pso",   "cb",    "sc",  "ia",    "call",   "res",
    "clr",  "rec",  "sub",   "pres",  "fence", "drain", "pace", "onf"};
constexpr const char *kCountNames[kCounts] = {
    "upd",  "cbblocks", "scup", "psob",    "resolves", "ghashKB", "gringKB",
    "gnew", "gnewKB",   "upKB", "unlocks", "walked",   "lockmiss", "cbregs", "scslots",
    "gres", "gsamp",    "gtrust", "ghsmKB", "ghunKB",  "gmiss"};
constexpr bool kCountIsBytes[kCounts] = {false, false, false, false, false, true, true,
                                         false, true,  true,  false, false, false, false, false,
                                         false, false, false, true,  true,  false};
// Below this many draws a frame the scene is a menu: nothing to learn, no line.
constexpr double kPerfMinDraws = 400.0;

thread_local int t_zone = 0;
thread_local uint64_t t_zone_last = 0;
thread_local uint64_t t_zone_ticks[kZones] = {};
thread_local int t_other_depth = 0;
thread_local uint64_t t_other_start = 0;

std::atomic<uint64_t> g_perf_counts[kCounts];
std::atomic<uint64_t> g_perf_other_ticks{0};  // zones on threads other than the frame thread
std::atomic<uint64_t> g_perf_gpu_ns{0};
std::atomic<uint32_t> g_perf_gpu_frames{0};
std::atomic<uint64_t> g_perf_render_ticks{0};

}  // namespace

void SvrPerfCountAdd(SvrCount what, uint64_t n) {
  g_perf_counts[size_t(what)].fetch_add(n, std::memory_order_relaxed);
}

void SvrPerfGpuTime(uint64_t nanoseconds) {
  g_perf_gpu_ns.fetch_add(nanoseconds, std::memory_order_relaxed);
  g_perf_gpu_frames.fetch_add(1, std::memory_order_relaxed);
}

void SvrPerfRenderFn(uint64_t ticks) {
  g_perf_render_ticks.fetch_add(ticks, std::memory_order_relaxed);
}

int SvrZoneEnter(SvrZoneId id) {
  if (!t_frame_thread) {
    if (t_other_depth++ == 0)
      t_other_start = SvrDiagTicks();
    return -1;
  }
  const uint64_t now = SvrDiagTicks();
  t_zone_ticks[t_zone] += now - t_zone_last;
  t_zone_last = now;
  const int previous = t_zone;
  t_zone = int(id);
  return previous;
}

void SvrZoneLeave(int previous) {
  if (previous < 0) {
    if (--t_other_depth == 0)
      g_perf_other_ticks.fetch_add(SvrDiagTicks() - t_other_start, std::memory_order_relaxed);
    return;
  }
  const uint64_t now = SvrDiagTicks();
  t_zone_ticks[t_zone] += now - t_zone_last;
  t_zone_last = now;
  t_zone = previous;
}

void SvrZoneSwitch(SvrZoneId id) {
  if (!t_frame_thread)
    return;
  const uint64_t now = SvrDiagTicks();
  t_zone_ticks[t_zone] += now - t_zone_last;
  t_zone_last = now;
  t_zone = int(id);
}

namespace {

// NR-12: the GPU's own view, read from NVML (nvml.dll ships with the NVIDIA driver) once a
// [nperf] line. The timestamp `gpu` time alone can't size pixel work: a lightly loaded laptop GPU
// drops its clock, so the same frame takes about the same milliseconds at every scale. Busy % and
// the clock against its maximum give the headroom; memory is the whole card's (VRAM).
struct NvmlUtil { unsigned int gpu, memory; };
struct NvmlMemory { unsigned long long total, free, used; };
using NvmlDevice = void *;

struct Nvml {
  int (*device_utilization)(NvmlDevice, NvmlUtil *) = nullptr;
  int (*device_clock)(NvmlDevice, int, unsigned int *) = nullptr;
  int (*device_max_clock)(NvmlDevice, int, unsigned int *) = nullptr;
  int (*device_memory)(NvmlDevice, NvmlMemory *) = nullptr;
  int (*device_power)(NvmlDevice, unsigned int *) = nullptr;
  NvmlDevice device = nullptr;
};

const Nvml *SvrNvml() {
  static const Nvml *s_nvml = []() -> const Nvml * {
#if defined(_WIN32)
    HMODULE dll = LoadLibraryA("nvml.dll");
    if (!dll)
      dll = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if (!dll)
      return nullptr;
    auto init = reinterpret_cast<int (*)()>(GetProcAddress(dll, "nvmlInit_v2"));
    auto by_index = reinterpret_cast<int (*)(unsigned int, NvmlDevice *)>(
        GetProcAddress(dll, "nvmlDeviceGetHandleByIndex_v2"));
    auto *n = new Nvml;
    n->device_utilization = reinterpret_cast<decltype(n->device_utilization)>(
        GetProcAddress(dll, "nvmlDeviceGetUtilizationRates"));
    n->device_clock =
        reinterpret_cast<decltype(n->device_clock)>(GetProcAddress(dll, "nvmlDeviceGetClockInfo"));
    n->device_max_clock = reinterpret_cast<decltype(n->device_max_clock)>(
        GetProcAddress(dll, "nvmlDeviceGetMaxClockInfo"));
    n->device_memory =
        reinterpret_cast<decltype(n->device_memory)>(GetProcAddress(dll, "nvmlDeviceGetMemoryInfo"));
    n->device_power =
        reinterpret_cast<decltype(n->device_power)>(GetProcAddress(dll, "nvmlDeviceGetPowerUsage"));
    if (!init || !by_index || !n->device_utilization || !n->device_clock || !n->device_max_clock ||
        !n->device_memory || !n->device_power || init() != 0 || by_index(0, &n->device) != 0) {
      BD_INFO("[nperf] nvml not available: no busy/clock/VRAM columns");
      return nullptr;
    }
    return n;
#else
    return nullptr;
#endif
  }();
  return s_nvml;
}

// NR-15b: on a hybrid laptop rendering on the iGPU, these calls woke the sleeping NVIDIA card
// and blocked 300-580 ms, once a second, on the frame thread. Now a thread of its own reads them
// once a second and the [nperf] line takes the last answer; with a non-NVIDIA render adapter
// NVML isn't touched at all. 0 = adapter not reported (Vulkan): read as before.
std::atomic<uint32_t> g_render_vendor{0};
constexpr uint32_t kVendorNvidia = 0x10DE;

struct NvmlCache {
  std::mutex mutex;
  std::string columns;
};

// " | busy 41 clk 1350 maxclk 2370 vramMB 2140 W 38.2", or "" without NVML.
std::string SvrNvmlRead(const Nvml *n) {
  NvmlUtil util{};
  NvmlMemory mem{};
  unsigned int clk = 0, max_clk = 0, mw = 0;
  n->device_utilization(n->device, &util);
  n->device_clock(n->device, 0, &clk);  // NVML_CLOCK_GRAPHICS
  n->device_max_clock(n->device, 0, &max_clk);
  n->device_memory(n->device, &mem);
  n->device_power(n->device, &mw);
  return fmt::format(" | busy {} clk {} maxclk {} vramMB {} W {:.1f}", util.gpu, clk,
                     max_clk, mem.used >> 20, mw / 1000.0);
}

// Never blocks: the first call starts the reader, so the first line has no columns.
std::string SvrNvmlColumns() {
  const uint32_t vendor = g_render_vendor.load(std::memory_order_relaxed);
  if (vendor != 0 && vendor != kVendorNvidia) {
    static const bool s_logged = [vendor] {
      BD_INFO("[nperf] render adapter vendor 0x{:04X} isn't NVIDIA: no busy/clock/VRAM columns",
              vendor);
      return true;
    }();
    (void)s_logged;
    return {};
  }
  // Leaked on purpose: the reader thread is detached and may outlive static destruction.
  static NvmlCache *s_cache = [] {
    auto *cache = new NvmlCache;
    std::thread([cache] {
#if defined(_WIN32)
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      SetThreadDescription(GetCurrentThread(), L"ufc3 nperf nvml");
#endif
      const Nvml *n = SvrNvml();
      if (!n)
        return;
      for (;;) {
        std::string columns = SvrNvmlRead(n);
        {
          std::lock_guard lock(cache->mutex);
          cache->columns = std::move(columns);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }).detach();  // process exit is a hard kill, never joined
    return cache;
  }();
  std::lock_guard lock(s_cache->mutex);
  return s_cache->columns;
}

}  // namespace

void SvrPerfRenderAdapter(uint32_t vendor_id) {
  g_render_vendor.store(vendor_id, std::memory_order_relaxed);
}

// NR-15: weak-GPU stand-in on a dedicated-mode laptop (the iGPU is hidden there): lock the NVIDIA
// card's graphics (and memory) clock low for the whole run. Needs admin; NVML says no otherwise.
// A run killed before exit leaves the lock on, so a marker file makes the next start reset it.
void SvrTestGpuClocks(unsigned int gpu_mhz, unsigned int mem_mhz) {
#if defined(_WIN32)
  char temp[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, temp);
  static std::string marker = std::string(temp) + "ufc3_test_gpu_locked.flag";
  const bool was_locked = GetFileAttributesA(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
  if (!gpu_mhz && !mem_mhz && !was_locked)
    return;
  const Nvml *n = SvrNvml();
  HMODULE dll = GetModuleHandleA("nvml.dll");
  if (!dll)
    dll = GetModuleHandleA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
  if (!n || !dll) {
    BD_INFO("[nr15] test GPU clocks: nvml not available");
    return;
  }
  using Lock = int (*)(NvmlDevice, unsigned int, unsigned int);
  using Reset = int (*)(NvmlDevice);
  static Lock lock_gpu = reinterpret_cast<Lock>(GetProcAddress(dll, "nvmlDeviceSetGpuLockedClocks"));
  static Lock lock_mem = reinterpret_cast<Lock>(GetProcAddress(dll, "nvmlDeviceSetMemoryLockedClocks"));
  static Reset reset_gpu = reinterpret_cast<Reset>(GetProcAddress(dll, "nvmlDeviceResetGpuLockedClocks"));
  static Reset reset_mem = reinterpret_cast<Reset>(GetProcAddress(dll, "nvmlDeviceResetMemoryLockedClocks"));
  static NvmlDevice device = n->device;
  if (was_locked) {
    const int rg = reset_gpu ? reset_gpu(device) : -1;
    const int rm = reset_mem ? reset_mem(device) : -1;
    DeleteFileA(marker.c_str());
    BD_INFO("[nr15] test GPU clocks: reset a lock left by an earlier run (nvml {} / {})", rg, rm);
  }
  if (!gpu_mhz && !mem_mhz)
    return;
  const int rg = gpu_mhz && lock_gpu ? lock_gpu(device, gpu_mhz, gpu_mhz) : -1;
  const int rm = mem_mhz && lock_mem ? lock_mem(device, mem_mhz, mem_mhz) : -1;
  if (rg == 0 || rm == 0) {
    if (HANDLE f = CreateFileA(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        f != INVALID_HANDLE_VALUE)
      CloseHandle(f);
    std::atexit([] {
      if (reset_gpu)
        reset_gpu(device);
      if (reset_mem)
        reset_mem(device);
      DeleteFileA(marker.c_str());
    });
  }
  BD_INFO("[nr15] test GPU clocks: graphics {} MHz nvml {}, memory {} MHz nvml {} (0 = locked, 4 = "
          "needs admin, -1 = not asked)",
          gpu_mhz, rg, mem_mhz, rm);
#endif
}

namespace {

// Once per frame, on the frame thread.
void SvrPerfTick(Clock::time_point now, double frame_ms, uint32_t draws) {
  if (!REXCVAR_GET(ufc3_native_perf))
    return;
  static Clock::time_point start{};
  static std::vector<float> frames;
  static uint64_t window_draws = 0;
  if (!g_svr_perf_on.load(std::memory_order_relaxed)) {
    frames.reserve(512);
    start = now;
    t_zone = 0;
    t_zone_last = SvrDiagTicks();
    std::fill(std::begin(t_zone_ticks), std::end(t_zone_ticks), uint64_t(0));
    g_svr_perf_on.store(true, std::memory_order_relaxed);
    return;
  }
  frames.push_back(float(frame_ms));
  window_draws += draws;
  if (now - start < std::chrono::seconds(1))
    return;

  // Close the running zone so the window's ticks add up to the window.
  const uint64_t tick_now = SvrDiagTicks();
  t_zone_ticks[t_zone] += tick_now - t_zone_last;
  t_zone_last = tick_now;
  uint64_t total_ticks = 0;
  for (uint64_t ticks : t_zone_ticks)
    total_ticks += ticks;
  const double wall_ms = std::chrono::duration<double, std::milli>(now - start).count();
  const double n = double(frames.size());
  const double ms_per_tick = total_ticks ? wall_ms / double(total_ticks) : 0.0;
  const double draws_per_frame = double(window_draws) / n;

  const uint64_t other = g_perf_other_ticks.exchange(0, std::memory_order_relaxed);
  const uint64_t render = g_perf_render_ticks.exchange(0, std::memory_order_relaxed);
  const uint64_t gpu_ns = g_perf_gpu_ns.exchange(0, std::memory_order_relaxed);
  const uint32_t gpu_frames = g_perf_gpu_frames.exchange(0, std::memory_order_relaxed);
  uint64_t counts[kCounts];
  for (size_t i = 0; i < kCounts; ++i)
    counts[i] = g_perf_counts[i].exchange(0, std::memory_order_relaxed);

  if (draws_per_frame >= kPerfMinDraws) {
    std::sort(frames.begin(), frames.end());
    const auto at = [&](double q) { return double(frames[size_t(q * (n - 1.0))]); };
    std::string line = fmt::format(
        "[nperf] fps {:.1f} avg {:.2f} p50 {:.1f} p95 {:.1f} max {:.1f} draws {:.0f} gpu {:.2f} "
        "renderfn {:.2f} other {:.2f} |",
        n * 1000.0 / wall_ms, wall_ms / n, at(0.5), at(0.95), double(frames.back()),
        draws_per_frame, gpu_frames ? double(gpu_ns) / double(gpu_frames) / 1e6 : 0.0,
        double(render) * ms_per_tick / n, double(other) * ms_per_tick / n);
    for (size_t i = 0; i < kZones; ++i)
      fmt::format_to(std::back_inserter(line), " {} {:.2f}", kZoneNames[i],
                     double(t_zone_ticks[i]) * ms_per_tick / n);
    line += " |";
    for (size_t i = 0; i < kCounts; ++i)
      fmt::format_to(std::back_inserter(line), " {} {:.0f}", kCountNames[i],
                     double(counts[i]) / n / (kCountIsBytes[i] ? 1024.0 : 1.0));
    line += SvrNvmlColumns();
    BD_INFO("{}", line);
  }

  std::fill(std::begin(t_zone_ticks), std::end(t_zone_ticks), uint64_t(0));
  frames.clear();
  window_draws = 0;
  start = now;
}

}  // namespace

void SvrDiagEndFrame() {
  // First frame: cvars are parsed and the trace folder is set by now.
  static const bool s_stall_trace = [] {
    if (const int ms = REXCVAR_GET(ufc3_stall_trace_ms); ms > 0)
      SvrStallTraceStart(ms);
    return true;
  }();
  (void)s_stall_trace;
  static Clock::time_point last{};
  static uint64_t frames = 0, logged = 0;
  static double window_max = 0.0;
  static uint32_t window_slow = 0, window_frames = 0;
  static Clock::time_point window_start = Clock::now();

  const auto now = Clock::now();
  const double frame_ms =
      last == Clock::time_point{} ? 0.0 : std::chrono::duration<double, std::milli>(now - last).count();
  last = now;
  ++frames;

  // Timer ticks per microsecond, measured against the OS clock since the first frame.
  static const uint64_t ticks0 = SvrDiagTicks();
  static const Clock::time_point clock0 = now;
  const double elapsed_us = std::chrono::duration<double, std::micro>(now - clock0).count();
  const double ticks_per_us =
      elapsed_us > 0.0 ? double(SvrDiagTicks() - ticks0) / elapsed_us : 0.0;

  uint64_t us[size_t(SvrDiag::kCount)];
  uint32_t n[size_t(SvrDiag::kCount)];
  t_frame_thread = true;
  for (size_t i = 0; i < g_slots.size(); ++i) {
    us[i] = g_slots[i].us.exchange(0, std::memory_order_relaxed);
    const uint64_t ticks =
        g_slots[i].ticks.exchange(0, std::memory_order_relaxed) + t_local[i].ticks;
    if (ticks_per_us > 0.0)
      us[i] += uint64_t(double(ticks) / ticks_per_us);
    n[i] = g_slots[i].n.exchange(0, std::memory_order_relaxed) + t_local[i].n;
    t_local[i] = {};
  }

  SvrPerfTick(now, frame_ms, n[size_t(SvrDiag::kDraw)]);

  const int threshold = REXCVAR_GET(svr_frame_diag_ms);
  if (threshold <= 0 || frames < 120)  // skip startup
    return;

  static uint64_t window_draws = 0, window_draw_us = 0, window_check_us = 0, window_hash = 0,
                  window_gpu_wait_us = 0;
  window_gpu_wait_us += us[size_t(SvrDiag::kGuestGpuWait)];
  ++window_frames;
  window_draws += n[size_t(SvrDiag::kDraw)];
  window_draw_us += us[size_t(SvrDiag::kDraw)];
  window_check_us += us[size_t(SvrDiag::kTextureCheck)];
  window_hash += us[size_t(SvrDiag::kHashBytes)];
  if (frame_ms > window_max)
    window_max = frame_ms;
  if (frame_ms > threshold) {
    ++window_slow;
    if (logged < 2000) {
      ++logged;
      auto at = [&](SvrDiag d) { return size_t(d); };
      BD_INFO("[hitch] {:.1f} ms frame: pipelines {}x {:.1f} ms | new textures {}x {:.1f} ms | "
              "texture checks {:.1f} ms | new targets {}x {:.1f} ms | GPU wait {:.1f} ms | "
              "swapchain {:.1f} ms | draws {}x {:.1f} ms | {} texture checks hashing {:.1f} MB | "
              "game waiting for emulated GPU {}x {:.1f} ms | target fills {}x {:.1f} ms | "
              "hashing {:.1f} ms | mirror creates {}x {:.1f} ms | mirror uploads {:.1f} ms | "
              "untile {:.1f} ms | upload chunks made {}x {:.1f} ms",
              frame_ms, n[at(SvrDiag::kPipelineCompile)], Ms(us[at(SvrDiag::kPipelineCompile)]),
              n[at(SvrDiag::kTextureMirror)], Ms(us[at(SvrDiag::kTextureMirror)]),
              Ms(us[at(SvrDiag::kTextureCheck)]), n[at(SvrDiag::kTargetCreate)],
              Ms(us[at(SvrDiag::kTargetCreate)]), Ms(us[at(SvrDiag::kGpuWait)]),
              Ms(us[at(SvrDiag::kAcquire)]), n[at(SvrDiag::kDraw)], Ms(us[at(SvrDiag::kDraw)]),
              n[at(SvrDiag::kTextureCheck)], double(us[at(SvrDiag::kHashBytes)]) / (1024.0 * 1024.0),
              n[at(SvrDiag::kGuestGpuWait)], Ms(us[at(SvrDiag::kGuestGpuWait)]),
              n[at(SvrDiag::kTargetFill)], Ms(us[at(SvrDiag::kTargetFill)]),
              Ms(us[at(SvrDiag::kTextureHash)]), n[at(SvrDiag::kMirrorCreate)],
              Ms(us[at(SvrDiag::kMirrorCreate)]), Ms(us[at(SvrDiag::kMirrorUpload)]),
              Ms(us[at(SvrDiag::kMirrorUntile)]), n[at(SvrDiag::kUploadChunk)],
              Ms(us[at(SvrDiag::kUploadChunk)]));
    }
  }
  if (now - window_start >= std::chrono::seconds(10)) {
    const double f = window_frames ? double(window_frames) : 1.0;
    BD_INFO("[frames] last 10 s: {} frames, slowest {:.1f} ms, {} over {} ms | per frame: "
            "{:.0f} draws in {:.1f} ms, texture checks {:.1f} ms hashing {:.1f} MB, waiting for "
            "emulated GPU {:.2f} ms",
            window_frames, window_max, window_slow, threshold, window_draws / f,
            Ms(window_draw_us) / f, Ms(window_check_us) / f,
            double(window_hash) / f / (1024.0 * 1024.0), Ms(window_gpu_wait_us) / f);
    window_draws = window_draw_us = window_check_us = window_hash = window_gpu_wait_us = 0;
    window_start = now;
    window_max = 0.0;
    window_slow = 0;
    window_frames = 0;
  }
}

}  // namespace bd::gpu
