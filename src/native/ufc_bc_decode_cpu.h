// NR-WHITEUI: DXT1/DXT3/DXT5 (BC1/BC2/BC3) blocks decoded to RGBA8 on the CPU.
// D3D12 creates a BC texture whose level-0 width or height isn't a multiple of 4 only when the
// runtime and driver report D3D12_OPTIONS8.UnalignedBlockTexturesSupported. Where they don't, the
// mirror is built as RGBA8 from these decoders instead, same size, so UVs don't change.
// Blocks are in GPU (little-endian) byte order, as after the mirror's endian swap. Standalone: no
// renderer or game dependency (the encoder next door is ufc_dxt5_cpu.h).

#pragma once

#include <cstddef>
#include <cstdint>

namespace ufc3_bc {

enum class Kind { kBC1, kBC2, kBC3 };

inline uint32_t BlockBytes(Kind k) { return k == Kind::kBC1 ? 8u : 16u; }

namespace detail {

inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline void Expand565(uint16_t c, uint8_t out[3]) {
  const uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
  out[0] = uint8_t((r << 3) | (r >> 2));
  out[1] = uint8_t((g << 2) | (g >> 4));
  out[2] = uint8_t((b << 3) | (b >> 2));
}

// The colour half: 16 texels of RGBA into px (alpha 255, or 0 for BC1's 3-colour transparent code).
inline void DecodeColour(const uint8_t* blk, bool allow_3colour, uint8_t px[16][4]) {
  const uint16_t c0 = Le16(blk), c1 = Le16(blk + 2);
  uint8_t pal[4][4];
  Expand565(c0, pal[0]);
  Expand565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  if (!allow_3colour || c0 > c1) {
    for (int c = 0; c < 3; ++c) {
      pal[2][c] = uint8_t((2u * pal[0][c] + pal[1][c] + 1u) / 3u);
      pal[3][c] = uint8_t((pal[0][c] + 2u * pal[1][c] + 1u) / 3u);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int c = 0; c < 3; ++c) {
      pal[2][c] = uint8_t((pal[0][c] + pal[1][c] + 1u) / 2u);
      pal[3][c] = 0;
    }
    pal[2][3] = 255;
    pal[3][3] = 0;
  }
  const uint32_t idx = Le32(blk + 4);
  for (int t = 0; t < 16; ++t) {
    const uint8_t* p = pal[(idx >> (2 * t)) & 3u];
    px[t][0] = p[0];
    px[t][1] = p[1];
    px[t][2] = p[2];
    px[t][3] = p[3];
  }
}

}  // namespace detail

// One block into 16 RGBA8 texels, row-major (t = 4 * row + col).
inline void DecodeBlock(Kind kind, const uint8_t* blk, uint8_t px[16][4]) {
  using namespace detail;
  switch (kind) {
  case Kind::kBC1:
    DecodeColour(blk, true, px);
    return;
  case Kind::kBC2: {
    DecodeColour(blk + 8, false, px);
    for (int t = 0; t < 16; ++t) {
      const uint32_t a = (blk[t >> 1] >> ((t & 1) * 4)) & 15u;
      px[t][3] = uint8_t(a * 17u);
    }
    return;
  }
  case Kind::kBC3: {
    DecodeColour(blk + 8, false, px);
    const uint32_t a0 = blk[0], a1 = blk[1];
    uint8_t pal[8];
    pal[0] = uint8_t(a0);
    pal[1] = uint8_t(a1);
    if (a0 > a1) {
      for (uint32_t i = 1; i < 7; ++i)
        pal[i + 1] = uint8_t(((7u - i) * a0 + i * a1 + 3u) / 7u);
    } else {
      for (uint32_t i = 1; i < 5; ++i)
        pal[i + 1] = uint8_t(((5u - i) * a0 + i * a1 + 2u) / 5u);
      pal[6] = 0;
      pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i)
      bits |= uint64_t(blk[2 + i]) << (8 * i);
    for (int t = 0; t < 16; ++t)
      px[t][3] = pal[(bits >> (3 * t)) & 7u];
    return;
  }
  }
}

// One level (or one volume slice): src rows of blocks at src_row_bytes, dst rows of RGBA8 texels at
// dst_row_bytes. Texels past width x height in edge blocks are dropped.
inline void DecodeLevel(Kind kind, const uint8_t* src, size_t src_size, uint32_t src_row_bytes,
                        uint32_t width, uint32_t height, uint8_t* dst, uint32_t dst_row_bytes) {
  const uint32_t bw = (width + 3u) / 4u, bh = (height + 3u) / 4u, bb = BlockBytes(kind);
  uint8_t px[16][4];
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const size_t so = size_t(by) * src_row_bytes + size_t(bx) * bb;
      if (so + bb > src_size)
        continue;  // the caller zero-fills dst
      DecodeBlock(kind, src + so, px);
      for (uint32_t ty = 0; ty < 4; ++ty) {
        const uint32_t y = by * 4u + ty;
        if (y >= height)
          break;
        for (uint32_t tx = 0; tx < 4; ++tx) {
          const uint32_t x = bx * 4u + tx;
          if (x >= width)
            break;
          uint8_t* o = dst + size_t(y) * dst_row_bytes + size_t(x) * 4u;
          const uint8_t* p = px[ty * 4u + tx];
          o[0] = p[0];
          o[1] = p[1];
          o[2] = p[2];
          o[3] = p[3];
        }
      }
    }
  }
}

}  // namespace ufc3_bc
