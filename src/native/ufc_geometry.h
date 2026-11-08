// SvR 2008: vertex and index buffers for the native renderer's draws.
//
// SvR's D3D (stock XDK 2.0.5632) keeps its buffers as its own guest objects:
//   D3DVertexBuffer (D3DDevice_CreateVertexBuffer sub_82235648): D3DResource + vertex fetch
//     constant: +0x18 = CPU address | 3, +0x1C = size in bytes (bits 2-25) | 0x10000000 | endian
//     (2 = 8in32).
//   D3DIndexBuffer (D3DDevice_CreateIndexBuffer sub_82235770): D3DResource (Common bit 31 =
//     INDEX32, bits 30-29 = endian: 1 = 8in16 for INDEX16, 2 = 8in32 for INDEX32), Address (+0x18,
//     CPU address), Size (+0x1C, exact bytes).
// The CPU addresses are in a physical alias (0xA0000000 / 0xC0000000 / 0xE0000000). The XDK turns
// them into GPU addresses where the GPU reads them: SetStreamSource (sub_8225A648) writes stream
// N's fetch constant into the device, DrawIndexedVertices (sub_825A4340) converts the index buffer
// address into its DRAW_INDX packet.
#pragma once

#include <vector>

#include <rex/types.h>

namespace bd::gpu {

// The XDK's GPU_CONVERT_CPU_TO_GPU_ADDRESS: a CPU address in any physical alias, or an address that
// already is a GPU one, to the GPU (physical) address. The 0xE0000000 alias (4 KB pages) maps 4 KB
// further on, so TranslatePhysical(cpu_address) alone reads the wrong page there.
constexpr u32 SvrGpuAddress(u32 address) {
  return (address & 0x1FFFFFFFu) + ((((address >> 20) + 0x200u) & 0x1000u));
}

// Binds the vertex streams the current vertex declaration reads and, for an indexed draw, the index
// buffer, as SvR's D3D left them in the device for the draw about to run. Call before taking
// state().mutex (the Video setters take it).
void SvrBindDrawGeometry(u32 device_guest, bool indexed);

// Draw log: stream 0's fetch constant and first vertices as guest memory holds them right now.
void SvrDrawLogStream0(u32 device_guest);

// UFC: D3D12 has no triangle fans (plume asserts on one), and UFC's menus draw them. A fan is drawn
// as an indexed triangle list instead: (first, i, i + 1) for each triangle. Both bind a 16- or
// 32-bit index buffer from the per-frame upload ring and return the number of indices to draw
// (0 = nothing to draw). Call after the vertex streams are bound and before state().mutex.
// Sequential: vertices 0..vertex_count-1 (BeginVertices, DrawVertices: start goes in base vertex).
u32 SvrBindFanIndices(u32 vertex_count);
// Indexed: fan_count indices of the device's index buffer, from start_index.
u32 SvrBindFanIndicesFromDevice(u32 device_guest, u32 start_index, u32 fan_count);
// Indexed quad list (Xenos type 13): index_count / 4 quads of the device's index buffer, from
// start_index, bound as a triangle list (a,b,c, a,c,d). Returns the index count to draw, 0 = not
// readable. Logs once per vertex shader what the indices and first corners were.
u32 SvrBindQuadIndicesFromDevice(u32 device_guest, u32 start_index, u32 index_count,
                                 i32 base_vertex, u64 vs_hash);

// For draws the hooks rebuild on the CPU (rectangle lists): stream 0's vertices [first, first +
// count) as the GPU reads them (host order, `stride` bytes each), and `count` indices of the
// device's index buffer from `start`. False = not readable (no buffer, out of range).
bool SvrReadStream0Vertices(u32 device_guest, u32 first, u32 count, std::vector<u8> &out,
                            u32 &stride);
bool SvrReadDeviceIndices(u32 device_guest, u32 start, u32 count, std::vector<u32> &out);
// The same for all of stream 0, up to max_bytes. False = not readable or bigger.
bool SvrReadWholeStream0(u32 device_guest, u32 max_bytes, std::vector<u8> &out, u32 &stride);

// Logs, once per buffer and count, a draw that reads the word-rounded end of its index buffer: the
// last index against the rest and stream 0's size, to tell real data from a read past the buffer.
void SvrNoteWordRoundedIndices(u32 device_guest, u32 primitive, u32 start, u32 count, u64 vs_hash,
                               u64 ps_hash);

// The CPU rewrote [address, address + size) (CPU or GPU address): copies over it are compared with
// guest memory again at their next use, even within the same frame.
void SvrInvalidateGeometry(u32 address, u32 size);

}  // namespace bd::gpu
