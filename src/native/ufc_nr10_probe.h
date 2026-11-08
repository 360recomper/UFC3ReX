// NR-10 probe (ufc3_native_nr10_log, log only): the game's DXT compressor and the resolves that
// feed it. See ufc_nr10_probe.cpp.

#pragma once

#include <cstdint>
#include <string>

namespace bd::gpu {
struct NativeMirrorSource;
}

bool UfcNr10On();
// <folder of the log file>\nr10_dump (made if missing): where probes leave their byte dumps.
std::string UfcNr10DumpDir();
// One D3DDevice_Resolve into a texture, in guest pixels (no source rect = the whole src_w x src_h).
void UfcNr10NoteResolve(uint32_t dest_va, uint32_t level, uint32_t face, uint32_t flags, bool have_rect,
                        int32_t left, int32_t top, int32_t right, int32_t bottom, int32_t dst_x, int32_t dst_y,
                        uint32_t src_w, uint32_t src_h);
// A texture mirrored from guest memory was checked for a draw (SvrResolveTexture, past its
// same-frame shortcut), or took the shortcut.
void UfcNr10NoteBind(uint32_t texture_va, const bd::gpu::NativeMirrorSource& src);
void UfcNr10NoteShortcut(uint32_t texture_va);
// Around the renderer's part of DrawIndexedVertices; only does something inside the compressor.
void UfcNr10NoteDraw(uint32_t dev, uint32_t prim, uint32_t count, bool after);
