// ufc3 - diagnostic wrappers around kernel/XAM imports.
//
// The SDK runtime exports each import as __imp__<Name>. Defining the same
// symbol here makes the game's calls land in this file first (a local
// definition wins over the import library); the wrapper logs the call and
// forwards to the runtime's export. Enabled with --ufc3_trace_imports.

#pragma once

#include <atomic>
#include <cstring>

#include <windows.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(ufc3_trace_imports, false, "ufc3",
                    "Log save/profile related XAM and kernel import calls");

namespace ufc3_trace {

using ImportFn = void (*)(PPCContext&, uint8_t*);

inline ImportFn Resolve(const char* name) {
  static HMODULE runtime = [] {
    HMODULE m = GetModuleHandleA("rexruntimerd.dll");
    return m ? m : GetModuleHandleA("rexruntime.dll");
  }();
  return reinterpret_cast<ImportFn>(GetProcAddress(runtime, name));
}

inline void Call(const char* name, ImportFn& fn, std::atomic<int>& count, PPCContext& ctx,
                 uint8_t* base) {
  if (!fn) fn = Resolve(name);
  if (!REXCVAR_GET(ufc3_trace_imports) || count.fetch_add(1) >= 400) {
    fn(ctx, base);
    return;
  }
  uint32_t lr = static_cast<uint32_t>(ctx.lr);
  uint32_t a[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  fn(ctx, base);
  REXLOG_INFO("[imp] {}({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}) lr={:08X} -> {:08X}",
              name + 7, a[0], a[1], a[2], a[3], a[4], a[5], lr, ctx.r3.u32);
}

}  // namespace ufc3_trace

#define UFC3_TRACE_IMPORT(name)                                           \
  extern "C" REX_FUNC(__imp__##name) {                                    \
    static ufc3_trace::ImportFn fn = nullptr;                             \
    static std::atomic<int> count{0};                                     \
    ufc3_trace::Call("__imp__" #name, fn, count, ctx, base);              \
  }

UFC3_TRACE_IMPORT(XamShowSigninUI)
UFC3_TRACE_IMPORT(XamShowDeviceSelectorUI)
UFC3_TRACE_IMPORT(XamShowMessageBoxUI)
UFC3_TRACE_IMPORT(XamShowMessageBoxUIEx)
UFC3_TRACE_IMPORT(XamUserCheckPrivilege)
UFC3_TRACE_IMPORT(XamUserGetSigninInfo)
UFC3_TRACE_IMPORT(XamUserReadProfileSettings)
UFC3_TRACE_IMPORT(XamUserWriteProfileSettings)
UFC3_TRACE_IMPORT(XamContentCreateEx)
UFC3_TRACE_IMPORT(XamContentDelete)
UFC3_TRACE_IMPORT(XamContentClose)
UFC3_TRACE_IMPORT(XamContentFlush)
UFC3_TRACE_IMPORT(XamContentGetCreator)
UFC3_TRACE_IMPORT(XamContentGetLicenseMask)
UFC3_TRACE_IMPORT(XamContentGetDeviceState)
UFC3_TRACE_IMPORT(XamContentGetDeviceData)
UFC3_TRACE_IMPORT(XamTaskSchedule)
UFC3_TRACE_IMPORT(XamGetExecutionId)
UFC3_TRACE_IMPORT(XeKeysConsoleSignatureVerification)
UFC3_TRACE_IMPORT(XeKeysConsolePrivateKeySign)
UFC3_TRACE_IMPORT(NtWriteFile)
UFC3_TRACE_IMPORT(NtQueryVolumeInformationFile)
UFC3_TRACE_IMPORT(NtQueryFullAttributesFile)
UFC3_TRACE_IMPORT(NtFlushBuffersFile)
UFC3_TRACE_IMPORT(NtDeviceIoControlFile)
UFC3_TRACE_IMPORT(StfsControlDevice)
UFC3_TRACE_IMPORT(StfsCreateDevice)
UFC3_TRACE_IMPORT(ExGetXConfigSetting)
UFC3_TRACE_IMPORT(XNetLogonGetMachineID)
UFC3_TRACE_IMPORT(XMsgStartIORequest)
UFC3_TRACE_IMPORT(XMsgStartIORequestEx)
UFC3_TRACE_IMPORT(XMsgInProcessCall)

// Save-manager operation dispatcher (r4 = operation index into a 0x49-entry
// jump table: 0 = create save, 1 = save, ...).
REX_EXTERN(__imp__sub_82267178);
extern "C" REX_FUNC(sub_82267178) {
  static std::atomic<int> count{0};
  bool log = REXCVAR_GET(ufc3_trace_imports) && count.fetch_add(1) < 2000;
  uint32_t self = ctx.r3.u32, op = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32;
  uint32_t lr = static_cast<uint32_t>(ctx.lr);
  if (log) REXLOG_INFO("[save] op={:02X} enter lr={:08X}", op, lr);
  __imp__sub_82267178(ctx, base);
  if (log) {
    REXLOG_INFO("[save] op={:02X} this={:08X} r5={:08X} r6={:08X} lr={:08X} -> {:08X}", op, self,
                a5, a6, lr, ctx.r3.u32);
  }
}

namespace ufc3_trace {
inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}
}  // namespace ufc3_trace

// Save manager: request gate (vtable +0x58). r4 = operation kind; returns 0
// to drop the request and leaves an error code at this+0x57B4.
REX_EXTERN(__imp__sub_82268818);
extern "C" REX_FUNC(sub_82268818) {
  static std::atomic<int> count{0};
  bool log = REXCVAR_GET(ufc3_trace_imports) && count.fetch_add(1) < 2000;
  uint32_t self = ctx.r3.u32, op = ctx.r4.u32, lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82268818(ctx, base);
  if (log) {
    using ufc3_trace::Load32;
    REXLOG_INFO("[save] gate op={:02X} lr={:08X} -> {} err={} dev={:08X} 6898={:08X} 67f8={:08X}",
                op, lr, ctx.r3.u32, Load32(base, self + 0x57B4), Load32(base, self + 0x6890),
                Load32(base, self + 0x6898), Load32(base, self + 0x67F8));
  }
}

#define UFC3_TRACE_SAVE_REQUEST(addr)                                                    \
  REX_EXTERN(__imp__sub_##addr);                                                         \
  extern "C" REX_FUNC(sub_##addr) {                                                      \
    uint32_t r4 = ctx.r4.u32, r5 = ctx.r5.u32, lr = static_cast<uint32_t>(ctx.lr);      \
    __imp__sub_##addr(ctx, base);                                                        \
    if (REXCVAR_GET(ufc3_trace_imports)) {                                               \
      REXLOG_INFO("[save] request " #addr "(r4={:08X}, r5={:08X}) lr={:08X} -> {:08X}", \
                  r4, r5, lr, ctx.r3.u32);                                               \
    }                                                                                    \
  }

UFC3_TRACE_SAVE_REQUEST(82263090)
UFC3_TRACE_SAVE_REQUEST(82263138)
UFC3_TRACE_SAVE_REQUEST(822631E0)
UFC3_TRACE_SAVE_REQUEST(82263290)
UFC3_TRACE_SAVE_REQUEST(82263338)
UFC3_TRACE_SAVE_REQUEST(822623B0)

// 82EE9128 (save wrapper request) is hooked in legacy_roster.h, which logs it too.
