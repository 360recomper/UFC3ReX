// NR-10c: the game's GPU DXT5 compressor on the CPU. See ufc_memexport_cpu.cpp.

#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

struct UfcCpuCompressReport {
  uint32_t blocks = 0;
  double encode_ms = 0.0, source_ms = 0.0;
};

// Called at the top of the sub_82793B10 hook with its arguments in ctx. True: the level is done (or
// can't be) and the game's body must not run; ctx's volatile registers are spent. False: nothing
// was touched, run the body.
bool UfcCpuCompress(PPCContext& ctx, uint8_t* base, UfcCpuCompressReport* report);
