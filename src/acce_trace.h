// ufc3 - trace the walkout-gear (acce.mpc) loads (--ufc3_trace_acce, default off).
// FINDINGS-shirt-crash.md "Crash 4": Houston vs Jardine crashed at fight load with a
// memcpy to NULL in sub_827A49E0 (the chunk parser), parsing a buffer that held no
// file bytes. This logs every step that buffer passes through:
//
//   [acce] folder      sub_8224CC08(folder, name): folder load start. The archive sums
//                      the children's sizes, allocates the buffer (+84) and starts an
//                      async read (handle +48) in read-from-file mode.
//   [acce] folder done sub_8224BF30(folder) when it first reports done: buffer and
//                      its first 16 bytes (a model entry starts "YZLI", blta "DUMY").
//   [acce] model       sub_827A4C50 called from sub_82C48270 (the acce entry loop):
//                      fighter ID, slot, entry name (the caller's r1+96), entry
//                      address/size and first 16 bytes; then the buffer it hands to
//                      sub_827A49E0 with its first chunk {tag, size} (BAD = a size no
//                      file has), and the result.
//   [acce] havok       sub_82C467C8 (Havok/yhc entries): the same, before the call.
//   [acce] inflate     sub_827A20D8(src, src_len, dst, dst_len) on the decompress worker:
//                      every call while an acce model load waits on it, and every
//                      failure (return 0; the worker ignores it and the buffer keeps
//                      stale heap bytes).
//
// Folder lines are limited to acce/figacce names (cloth, hat, ts1/tc0 folders) unless
// --ufc3_trace_acce_all_folders. At most kMaxLines lines per run.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(ufc3_trace_acce, false, "ufc3",
                    "Log walkout-gear (acce.mpc) folder reads, model/Havok entry loads and inflate results "
                    "(fight-load crash 4, FINDINGS-shirt-crash.md)");
REXCVAR_DEFINE_BOOL(ufc3_trace_acce_all_folders, false, "ufc3",
                    "With --ufc3_trace_acce: log every archive folder load, not only acce/figacce ones");

REX_EXTERN(__imp__sub_8224CC08);  // archive folder load(folder, name) -> 1 started / 0 not found
REX_EXTERN(__imp__sub_8224BF30);  // folder load poll(folder) -> folder+96 (done)
REX_EXTERN(__imp__sub_827A4C50);  // model load(entry, size, heap, slot ptr): YZLI decompress + parse
REX_EXTERN(__imp__sub_827A49E0);  // chunk-file parser(buffer, 0, slot ptr, 0)
REX_EXTERN(__imp__sub_827A20D8);  // zlib inflate(src, src_len, dst, dst_len) -> bytes out, 0 = failed
REX_EXTERN(__imp__sub_82C467C8);  // acce Havok/yhc entry load(fighter, entry, size, slot ptr)

namespace ufc3_acce_trace {

constexpr uint32_t kMaxLines = 6000;
constexpr uint32_t kLrAcceLoop1 = 0x82C4840C;  // sub_82C48270's sub_827A4C50 calls
constexpr uint32_t kLrAcceLoop2 = 0x82C48708;
constexpr uint32_t kLrModelParse = 0x827A4D70;  // sub_827A4C50's sub_827A49E0 call
constexpr uint32_t kSlotBase = 444;            // fighter + 444 + 8 * slot

inline std::atomic<uint32_t> g_lines{0};
inline std::atomic<int32_t> g_models_waiting{0};  // acce model loads inside sub_827A4C50
inline thread_local bool t_in_acce_model = false;

inline bool Budget() { return g_lines.fetch_add(1, std::memory_order_relaxed) < kMaxLines; }

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}

inline uint16_t Load16(uint8_t* base, uint32_t addr) {
  uint16_t v;
  std::memcpy(&v, base + addr, 2);
  return _byteswap_ushort(v);
}

inline bool Plausible(uint32_t addr) { return addr >= 0x10000; }

inline std::string Name(uint8_t* base, uint32_t addr, size_t max = 32) {
  if (!Plausible(addr)) return "?";
  std::string s;
  for (size_t i = 0; i < max; ++i) {
    char c = static_cast<char>(base[addr + i]);
    if (!c) break;
    s += (c < 0x20 || c > 0x7E) ? '?' : c;
  }
  return s;
}

// 16 bytes as hex + ASCII.
inline std::string Head(uint8_t* base, uint32_t addr) {
  if (!Plausible(addr)) return "(null)";
  std::string hex, txt;
  for (uint32_t i = 0; i < 16; ++i) {
    uint8_t b = base[addr + i];
    hex += fmt::format("{:02X}", b);
    if (i % 4 == 3 && i != 15) hex += ' ';
    txt += (b < 0x20 || b > 0x7E) ? '.' : static_cast<char>(b);
  }
  return hex + " |" + txt + "|";
}

// fighter + 4 -> +0 -> +24 = fighter ID * 100 (as sub_82C47558 reads it).
inline int32_t FighterId(uint8_t* base, uint32_t fighter) {
  if (!Plausible(fighter)) return -1;
  uint32_t a = Load32(base, fighter + 4);
  if (!Plausible(a)) return -1;
  uint32_t b = Load32(base, a);
  if (!Plausible(b)) return -1;
  return static_cast<int32_t>(Load32(base, b + 24)) / 100;
}

inline bool AcceFolder(const std::string& n) {
  return REXCVAR_GET(ufc3_trace_acce_all_folders) || n.find("cloth") != std::string::npos || n == "hat" ||
         n.find("ts1") != std::string::npos || n.find("tc0") != std::string::npos ||
         (n.size() == 4 && (n[0] == '6' || n[0] == '9') && n.find_first_not_of("0123456789") == std::string::npos);
}

// Sum of a folder's child sizes (+92: u32 per child, bytes; count u16 at +2).
inline uint32_t FolderBytes(uint8_t* base, uint32_t folder) {
  uint32_t sizes = Load32(base, folder + 92), n = Load16(base, folder + 2), sum = 0;
  if (!Plausible(sizes) || n > 4096) return 0;
  for (uint32_t i = 0; i < n; ++i) sum += Load32(base, sizes + 4 * i);
  return sum;
}

}  // namespace ufc3_acce_trace

extern "C" REX_FUNC(sub_8224CC08) {
  namespace at = ufc3_acce_trace;
  if (!REXCVAR_GET(ufc3_trace_acce)) {
    __imp__sub_8224CC08(ctx, base);
    return;
  }
  const uint32_t folder = ctx.r3.u32, lr = static_cast<uint32_t>(ctx.lr);
  const std::string name = at::Name(base, ctx.r4.u32);
  __imp__sub_8224CC08(ctx, base);
  if (!at::AcceFolder(name) || !at::Budget()) return;
  const uint32_t archive = at::Load32(base, folder + 44);
  REXLOG_INFO("[acce] folder '{}' {:08X} (archive {:08X}, file base {:08X}, lr {:08X}): result {}, {} children, "
              "{} bytes, buffer {:08X}, read handle {:08X}",
              name, folder, archive, at::Plausible(archive) ? at::Load32(base, archive + 16) : 0, lr, ctx.r3.s32,
              at::Load16(base, folder + 2), at::FolderBytes(base, folder), at::Load32(base, folder + 84),
              at::Load32(base, folder + 48));
}

extern "C" REX_FUNC(sub_8224BF30) {
  namespace at = ufc3_acce_trace;
  if (!REXCVAR_GET(ufc3_trace_acce)) {
    __imp__sub_8224BF30(ctx, base);
    return;
  }
  const uint32_t folder = ctx.r3.u32;
  const uint32_t before = at::Load32(base, folder + 96);
  __imp__sub_8224BF30(ctx, base);
  if (before || !ctx.r3.u32) return;
  const std::string name = at::Name(base, folder + 52);
  if (!at::AcceFolder(name) || !at::Budget()) return;
  const uint32_t buf = at::Load32(base, folder + 84);
  REXLOG_INFO("[acce] folder done '{}' {:08X}: done {}, buffer {:08X} = {}", name, folder, ctx.r3.s32, buf,
              at::Head(base, buf));
}

extern "C" REX_FUNC(sub_827A4C50) {
  namespace at = ufc3_acce_trace;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  if (!REXCVAR_GET(ufc3_trace_acce) || (lr != at::kLrAcceLoop1 && lr != at::kLrAcceLoop2)) {
    __imp__sub_827A4C50(ctx, base);
    return;
  }
  // Caller state (sub_82C48270): r27 fighter, r25 folder, entry name at r1+96.
  const uint32_t entry = ctx.r3.u32, size = ctx.r4.u32, slot_ptr = ctx.r6.u32;
  const uint32_t fighter = ctx.r27.u32, folder = ctx.r25.u32;
  const int32_t slot = static_cast<int32_t>(slot_ptr - fighter - at::kSlotBase) / 8;
  const std::string name = at::Name(base, ctx.r1.u32 + 96);
  const int32_t fid = at::FighterId(base, fighter);
  if (at::Budget())
    REXLOG_INFO("[acce] model fighter {} ({:08X}) slot {} '{}' (folder '{}'): entry {:08X} size {} heap {} = {}", fid,
                fighter, slot, name, at::Name(base, folder + 52), entry, size, ctx.r5.s32, at::Head(base, entry));
  at::g_models_waiting.fetch_add(1, std::memory_order_relaxed);
  at::t_in_acce_model = true;
  __imp__sub_827A4C50(ctx, base);
  at::t_in_acce_model = false;
  at::g_models_waiting.fetch_sub(1, std::memory_order_relaxed);
  if (at::Budget())
    REXLOG_INFO("[acce] model fighter {} slot {} '{}': result {:08X}", fid, slot, name, ctx.r3.u32);
}

extern "C" REX_FUNC(sub_827A49E0) {
  namespace at = ufc3_acce_trace;
  if (at::t_in_acce_model && static_cast<uint32_t>(ctx.lr) == at::kLrModelParse && REXCVAR_GET(ufc3_trace_acce) &&
      at::Budget()) {
    const uint32_t buf = ctx.r3.u32;
    const uint32_t chunk = at::Plausible(buf) ? at::Load32(base, buf + 4) : 0;
    REXLOG_INFO("[acce]   parse buffer {:08X} = {}, first chunk size {:08X}{}", buf, at::Head(base, buf), chunk,
                chunk > 0x4000000 ? " BAD" : "");
  }
  __imp__sub_827A49E0(ctx, base);
}

extern "C" REX_FUNC(sub_827A20D8) {
  namespace at = ufc3_acce_trace;
  if (!REXCVAR_GET(ufc3_trace_acce)) {
    __imp__sub_827A20D8(ctx, base);
    return;
  }
  const uint32_t src = ctx.r3.u32, src_len = ctx.r4.u32, dst = ctx.r5.u32, dst_len = ctx.r6.u32;
  const bool waiting = at::g_models_waiting.load(std::memory_order_relaxed) > 0;
  __imp__sub_827A20D8(ctx, base);
  const uint32_t out = ctx.r3.u32;
  if ((waiting || out == 0) && at::Budget())
    REXLOG_INFO("[acce] inflate src {:08X} ({} bytes, {}) -> dst {:08X} ({} bytes): out {}{} = {}", src, src_len,
                at::Head(base, src), dst, dst_len, out, out == 0 ? " FAILED" : (out != dst_len ? " SHORT" : ""),
                at::Head(base, dst));
}

extern "C" REX_FUNC(sub_82C467C8) {
  namespace at = ufc3_acce_trace;
  if (REXCVAR_GET(ufc3_trace_acce) && at::Budget()) {
    const uint32_t fighter = ctx.r3.u32, entry = ctx.r4.u32;
    REXLOG_INFO("[acce] havok fighter {} ({:08X}) slot {} '{}': entry {:08X} size {} = {}", at::FighterId(base, fighter),
                fighter, static_cast<int32_t>(ctx.r6.u32 - fighter - at::kSlotBase) / 8,
                at::Name(base, ctx.r1.u32 + 96), entry, ctx.r5.u32, at::Head(base, entry));
  }
  __imp__sub_82C467C8(ctx, base);
}
