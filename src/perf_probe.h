// ufc3 - opt-in frame rate and wait-time probe (--ufc3_perf_probe).
//
// Wraps VdSwap and the kernel wait imports the same way import_trace.h does.
// Once a second it logs the guest frame rate (VdSwap calls) and, per guest
// thread, how much of that second was spent blocked in each wait import, so a
// thread that is busy (CPU-bound) can be told apart from one that is waiting.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <timeapi.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(ufc3_perf_probe, false, "ufc3",
                    "Log guest FPS and per-thread wait time once a second");

namespace ufc3_perf {

using ImportFn = void (*)(PPCContext&, uint8_t*);

inline ImportFn Resolve(const char* name) {
  static HMODULE runtime = [] {
    HMODULE m = GetModuleHandleA("rexruntimerd.dll");
    return m ? m : GetModuleHandleA("rexruntime.dll");
  }();
  return reinterpret_cast<ImportFn>(GetProcAddress(runtime, name));
}

enum Kind { kSwap, kWaitSingle, kWaitMulti, kNtWaitSingle, kNtWaitMulti, kDelay, kEnterCs,
            kKindCount };
inline const char* kKindNames[kKindCount] = {"VdSwap", "KeWaitSingle", "KeWaitMulti",
                                             "NtWaitSingle", "NtWaitMulti", "KeDelay", "EnterCS"};

constexpr int kSlots = 64;
struct Slot {
  std::atomic<uint32_t> tid{0};
  std::atomic<uint64_t> ns[kKindCount];
  std::atomic<uint32_t> calls[kKindCount];
  std::atomic<uint32_t> lr[kKindCount];
};
inline Slot g_slots[kSlots];
inline std::atomic<int> g_slot_count{0};
inline std::atomic<uint32_t> g_swaps{0};

inline uint64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline Slot* MySlot() {
  thread_local Slot* slot = [] {
    int i = g_slot_count.fetch_add(1);
    if (i >= kSlots) return static_cast<Slot*>(nullptr);
    g_slots[i].tid = GetCurrentThreadId();
    return &g_slots[i];
  }();
  return slot;
}

inline void Report() {
  static uint64_t last = NowNs();
  uint64_t now = NowNs();
  if (now - last < 1000000000ull) return;
  double secs = double(now - last) / 1e9;
  last = now;
  REXLOG_INFO("[perf] fps={:.1f}", g_swaps.exchange(0) / secs);
  int n = std::min(g_slot_count.load(), kSlots);
  for (int i = 0; i < n; ++i) {
    Slot& s = g_slots[i];
    std::string line;
    uint64_t total = 0;
    for (int k = 0; k < kKindCount; ++k) {
      uint64_t ns = s.ns[k].exchange(0);
      uint32_t c = s.calls[k].exchange(0);
      if (!c) continue;
      total += ns;
      line += fmt::format(" {}={:.0f}ms/{}@{:08X}", kKindNames[k], ns / 1e6 / secs, c,
                          s.lr[k].load());
    }
    if (!line.empty()) {
      REXLOG_INFO("[perf]   t{} blocked={:.0f}%{}", s.tid.load(), total / 1e7 / secs, line);
    }
  }
}

// Sampling profiler: every ~1 ms suspend the target thread (the VdSwap thread,
// or --ufc3_sample_tid), record its stack, and every 5 s log the hottest
// functions (leaf and inclusive), symbolized from ufc3.pdb.
inline void SamplerMain(DWORD target_tid) {
  HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                             FALSE, target_tid);
  if (!thread) return;
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE)) {
    REXLOG_WARN("[perf] SymInitialize failed: {}", GetLastError());
  }
  REXLOG_INFO("[perf] sampling host thread {}", target_tid);

  constexpr int kDepth = 12;
  std::vector<std::array<DWORD64, kDepth>> samples;
  samples.reserve(8192);
  std::unordered_map<DWORD64, std::string> names;
  auto name_of = [&](DWORD64 addr) -> const std::string& {
    auto it = names.find(addr);
    if (it != names.end()) return it->second;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256]{};
    auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 256;
    DWORD64 disp = 0;
    std::string n = SymFromAddr(GetCurrentProcess(), addr, &disp, si)
                        ? std::string(si->Name)
                        : fmt::format("?{:X}", addr);
    return names.emplace(addr, std::move(n)).first->second;
  };
  uint64_t window_start = NowNs();
  timeBeginPeriod(1);
  while (true) {
    Sleep(1);
    std::array<DWORD64, kDepth> st{};
    if (SuspendThread(thread) == DWORD(-1)) break;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &c)) {
      for (int d = 0; d < kDepth && c.Rip; ++d) {
        st[d] = c.Rip;
        DWORD64 image_base = 0;
        auto* fe = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
        if (!fe) {  // leaf function: return address on top of stack
          c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
          c.Rsp += 8;
          continue;
        }
        void* handler_data;
        DWORD64 establisher;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fe, &c, &handler_data,
                         &establisher, nullptr);
      }
    }
    ResumeThread(thread);
    samples.push_back(st);
    if (NowNs() - window_start < 5000000000ull) continue;
    window_start = NowNs();
    std::unordered_map<std::string, int> leaf, incl;
    for (auto& s : samples) {
      if (!s[0]) continue;
      leaf[name_of(s[0])]++;
      std::unordered_map<std::string, bool> seen;
      for (auto a : s) {
        if (!a) break;
        const std::string& n = name_of(a);
        if (!seen[n]) { seen[n] = true; incl[n]++; }
      }
    }
    auto dump = [&](const char* what, std::unordered_map<std::string, int>& m) {
      std::vector<std::pair<int, std::string>> v;
      for (auto& [n, k] : m) v.emplace_back(k, n);
      std::sort(v.rbegin(), v.rend());
      for (size_t i = 0; i < v.size() && i < 25; ++i) {
        REXLOG_INFO("[perf] {} {:5.1f}% {}", what, 100.0 * v[i].first / samples.size(),
                    v[i].second);
      }
    };
    REXLOG_INFO("[perf] ---- {} samples", samples.size());
    dump("leaf", leaf);
    dump("incl", incl);
    samples.clear();
  }
}

REXCVAR_DEFINE_INT32(ufc3_sample_tid, 0, "ufc3",
                     "Host thread id to sample with --ufc3_perf_probe (0: VdSwap thread, "
                     "-1: off)");

REXCVAR_DEFINE_STRING(ufc3_sample_thread, "", "ufc3",
                      "Sample the host thread whose name contains this instead "
                      "(e.g. \"GPU Commands\")");

inline DWORD FindThreadByName(const std::string& want) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 te{sizeof(te)};
  DWORD found = 0;
  for (BOOL ok = Thread32First(snap, &te); ok && !found; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
    PWSTR desc = nullptr;
    if (h && SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
      std::wstring w(desc);
      if (std::string(w.begin(), w.end()).find(want) != std::string::npos) {
        found = te.th32ThreadID;
      }
      LocalFree(desc);
    }
    if (h) CloseHandle(h);
  }
  CloseHandle(snap);
  return found;
}

inline void StartSampler() {
  static std::atomic<bool> started{false};
  if (REXCVAR_GET(ufc3_sample_tid) < 0 || started.exchange(true)) return;
  DWORD tid = REXCVAR_GET(ufc3_sample_tid) ? DWORD(REXCVAR_GET(ufc3_sample_tid))
                                           : GetCurrentThreadId();
  std::string want = REXCVAR_GET(ufc3_sample_thread);
  if (!want.empty()) tid = FindThreadByName(want);
  if (!tid) return;
  std::thread(SamplerMain, tid).detach();
}

inline void Call(Kind kind, const char* name, ImportFn& fn, PPCContext& ctx, uint8_t* base) {
  if (!fn) fn = Resolve(name);
  if (!REXCVAR_GET(ufc3_perf_probe)) {
    fn(ctx, base);
    return;
  }
  Slot* slot = MySlot();
  uint32_t lr = static_cast<uint32_t>(ctx.lr);
  uint64_t t0 = NowNs();
  fn(ctx, base);
  if (slot) {
    slot->ns[kind] += NowNs() - t0;
    slot->calls[kind]++;
    slot->lr[kind] = lr;
  }
  if (kind == kSwap) {
    StartSampler();
    g_swaps++;
    Report();
  }
}

}  // namespace ufc3_perf

#define UFC3_PERF_IMPORT(name, kind)                                       \
  extern "C" REX_FUNC(__imp__##name) {                                     \
    static ufc3_perf::ImportFn fn = nullptr;                               \
    ufc3_perf::Call(ufc3_perf::kind, "__imp__" #name, fn, ctx, base);      \
  }

UFC3_PERF_IMPORT(VdSwap, kSwap)
UFC3_PERF_IMPORT(KeWaitForSingleObject, kWaitSingle)
UFC3_PERF_IMPORT(KeWaitForMultipleObjects, kWaitMulti)
UFC3_PERF_IMPORT(NtWaitForSingleObjectEx, kNtWaitSingle)
UFC3_PERF_IMPORT(NtWaitForMultipleObjectsEx, kNtWaitMulti)
UFC3_PERF_IMPORT(KeDelayExecutionThread, kDelay)
UFC3_PERF_IMPORT(RtlEnterCriticalSection, kEnterCs)
