// Display facts for the native renderer's automatic internal resolution (svr_resources.cpp).
// Kept out of the renderer sources so <windows.h> doesn't meet re:Blue's headers.

#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

// Size of the primary display's current mode in physical pixels (not DPI-scaled).
bool SvrDisplaySize(uint32_t &w, uint32_t &h) {
#if defined(_WIN32)
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsHeight) {
    w = mode.dmPelsWidth;
    h = mode.dmPelsHeight;
    return true;
  }
#endif
  return false;
}

// NR-14e weaker-CPU stand-in (test only): keep the whole process on the logical CPUs in `mask`.
// Thread pools stay sized for the real CPU. Returns the mask now in force, 0 on failure.
uint64_t SvrSetTestCpuMask(uint64_t mask) {
#if defined(_WIN32)
  DWORD_PTR proc = 0, sys = 0;
  if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys))
    return 0;
  mask &= sys;
  if (!mask || !SetProcessAffinityMask(GetCurrentProcess(), static_cast<DWORD_PTR>(mask)))
    return 0;
  return mask;
#else
  return 0;
#endif
}

// NR-14e: CPU name (registry) and logical CPU count, for the perf reports. False if unknown.
bool SvrCpuName(char *name, uint32_t size, uint32_t &logical) {
#if defined(_WIN32)
  DWORD bytes = size;
  if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                   "ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &bytes) != ERROR_SUCCESS)
    return false;
  logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  return true;
#else
  return false;
#endif
}

// Steam sets SteamDeck=1 on the Deck, and Proton passes it through to the game.
bool SvrOnSteamDeck() {
  const char *v = std::getenv("SteamDeck");
  return v && std::strcmp(v, "1") == 0;
}
