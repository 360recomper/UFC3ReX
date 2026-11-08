// ufc3 - NR-12c: ask hybrid-graphics drivers for the dedicated GPU.
//
// NVIDIA Optimus and AMD PowerXpress / Switchable Graphics read these two exports from the
// process's .exe (not from a DLL) when it starts, and then default the game to the dedicated GPU
// instead of the integrated one that drives the laptop panel. A per-app choice in the NVIDIA
// Control Panel, AMD Software or Windows' graphics settings still overrides them. The native
// renderer also enumerates adapters in high-performance order (patches/plume-gpu-pick.patch), so
// this mostly matters for driver paths that ignore that order.
#if defined(_WIN32)
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
