// NR-8b-2k: the game's recorded command buffers on the native renderer (ufc_cb_probe.cpp).

#pragma once

#include <cstdint>

// True when the device is recording: the draw belongs to the buffer, its state was snapshotted, and
// nothing goes to the host now.
bool UfcCbCaptureDraw(uint32_t dev, bool indexed, uint32_t prim, uint32_t base_vertex, uint32_t start,
                      uint32_t count);
// The same test for draw paths that can't be replayed (BeginVertices): logged and dropped.
bool UfcCbRecordedDraw(uint32_t dev, const char* name, uint32_t prim, uint32_t count, uint32_t start,
                       int32_t base_vertex, uint64_t vs_hash, uint64_t ps_hash);

// hooks/draw.cpp: the normal draw hook, reading all its state from `dev`.
void UfcDrawRecorded(uint32_t dev, bool indexed, uint32_t prim, uint32_t base_vertex, uint32_t start,
                     uint32_t count);

// hooks/state.cpp: the renderer's Set* mirrors around a replay. kUfcMirrorTextures = the device's
// bound-texture table (kTexture, up to kViewport).
constexpr uint32_t kUfcMirrorTextures = 26;
void UfcMirrorSave();
void UfcMirrorApply(uint32_t vs, uint32_t ps, uint32_t decl, const uint32_t* textures);
void UfcMirrorRestore(uint32_t vs, uint32_t ps, uint32_t decl);
