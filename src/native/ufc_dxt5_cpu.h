// NR-10c: the game's GPU DXT5 compressor (yGPUCompression, PS 0afd5e7216525e4b) on the CPU.
// A port of encode() in tools/nr10_dxt5_ref.py, which reproduces the shader's bytes: same float32
// operations in the same order, so don't "simplify" (A + B) - A, the rcp terms or the 0 * x rule.
// No dependency on the renderer or the game, so tools/nr10_cpp_compare can build it alone.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

// The shader's rounding depends on these staying separate float32 operations.
#if defined(__clang__)
#pragma clang fp contract(off) reassociate(off)
#endif

namespace ufc3_dxt5 {

// Byte offset of (x, y) in a tiled 2D level: XGAddress2DTiledOffset, as nr10_dxt5_ref.tiled_offset.
// log2_bytes: 2 for a 4-byte texel, 4 for a 16-byte DXT5 block. x, y and pitch in texels or blocks.
inline uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log2_bytes + 7);
  const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         ((((((y & 8) >> 2) + (x >> 3)) & 3)) << 6) + (offset & 0x3F);
}

// A k_8_8_8_8 source level as its fetch constant describes it.
struct Source {
  const uint8_t* base = nullptr;
  size_t size = 0;
  uint32_t width = 0, height = 0;
  uint32_t pitch = 0;       // texels; only for a linear level
  bool tiled = true;
  uint32_t endian = 0;      // fetch dword 1 bits 6-7
  uint32_t swizzle = 0;     // fetch dword 3 bits 1-12
};

struct Dest {
  uint8_t* base = nullptr;  // the level's guest bytes
  size_t size = 0;
  uint32_t width_blocks = 0, height_blocks = 0;  // the whole level
};

struct Rect {
  uint32_t src_x = 0, src_y = 0;  // blocks, in the source level
  uint32_t dst_x = 0, dst_y = 0;  // blocks, in the destination level
  uint32_t width = 0, height = 0;  // blocks
};

inline const float* ByteToFloat() {
  static const float* table = [] {
    static float t[256];
    for (int i = 0; i < 256; ++i)
      t[i] = float(i) / 255.0f;
    return t;
  }();
  return table;
}

// One block. p[t][c], t = 4 * row + col; c = shader x, y, z, w (rgb = xyz, alpha = w). words: a1 * 256
// + a0, three alpha index words, c0, c1, two colour index words, as nr10_dxt5_ref.encode.
inline void EncodeBlock(const float p[16][4], uint16_t words[8]) {
  float min_c[4], max_c[4];
  for (int c = 0; c < 4; ++c) {
    float lo = p[0][c], hi = p[0][c];
    for (int t = 1; t < 16; ++t) {
      lo = p[t][c] < lo ? p[t][c] : lo;
      hi = p[t][c] > hi ? p[t][c] : hi;
    }
    min_c[c] = lo;
    max_c[c] = hi;
  }

  // Alpha half. Flat alpha: k is inf, every diff is 0, and the 0 * x = 0 rule makes each code 0.
  const float k = 7.0f * (1.0f / (max_c[3] - min_c[3]));
  const float a1 = std::floor(255.0f * min_c[3] + 0.5f);
  const float a0 = std::floor(255.0f * max_c[3] + 0.5f);
  uint64_t bits = 0;
  for (int t = 0; t < 16; ++t) {
    const float diff = max_c[3] - p[t][3];
    const float prod = diff == 0.0f ? 0.0f : k * diff;
    const float level = std::floor(prod + 0.5f);
    const float code = level == 0.0f ? 0.0f : level == 7.0f ? 1.0f : level + 1.0f;
    const int64_t icode = std::isfinite(code) ? int64_t(code) : 0;
    bits |= uint64_t(icode & 7) << (3 * t);
  }

  // Colour half.
  const float a[3] = {max_c[0], max_c[1], max_c[2]};
  const float b[3] = {(max_c[0] + min_c[0]) - a[0], min_c[1], (max_c[2] + min_c[2]) - a[2]};
  const auto pack = [](const float* c) {
    return (std::floor(31.0f * c[0] + 0.5f) * 64.0f + std::floor(63.0f * c[1] + 0.5f)) * 32.0f +
           std::floor(31.0f * c[2] + 0.5f);
  };
  const float c0 = pack(a), c1 = pack(b);
  const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
  const float s = 3.0f * (1.0f / (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
  int64_t lo = 0, hi = 0;
  if (c0 != c1) {
    for (int t = 0; t < 16; ++t) {
      const float dot =
          (p[t][0] - a[0]) * d[0] + (p[t][1] - a[1]) * d[1] + (p[t][2] - a[2]) * d[2];
      const float v = dot == 0.0f ? 0.0f : s * dot;
      const float level = std::floor(v + 0.5f);
      const float code = level == 0.0f ? 0.0f : level == 3.0f ? 1.0f : level + 1.0f;
      const int64_t icode = std::isfinite(code) ? int64_t(code) : 0;
      (t < 8 ? lo : hi) += icode * (int64_t(1) << (2 * (t & 7)));
    }
  }

  const auto word = [](int64_t v) { return uint16_t(v < 0 ? 0 : v > 65535 ? 65535 : v); };
  words[0] = word(int64_t(a1) * 256 + int64_t(a0));
  words[1] = uint16_t(bits & 0xFFFF);
  words[2] = uint16_t((bits >> 16) & 0xFFFF);
  words[3] = uint16_t((bits >> 32) & 0xFFFF);
  words[4] = word(int64_t(c0));
  words[5] = word(int64_t(c1));
  words[6] = word(lo);
  words[7] = word(hi);
}

// Reads the source level's texels in shader component order.
class SourceReader {
 public:
  explicit SourceReader(const Source& src) : src_(src) {
    // Byte i of the stored texel holds little-endian dword byte order_[i]'s value after the swap.
    static const uint8_t kOrder[4][4] = {{0, 1, 2, 3}, {1, 0, 3, 2}, {3, 2, 1, 0}, {2, 3, 0, 1}};
    for (int i = 0; i < 4; ++i) {
      order_[i] = kOrder[src.endian & 3][i];
      uint32_t pick = (src.swizzle >> (3 * i)) & 7;
      swizzle_[i] = pick > 5 ? 5 : pick;  // 4 = zero, 5 = one, as the tool
    }
  }

  // False when the texel is past the source bytes.
  bool Texel(uint32_t x, uint32_t y, float out[4]) const {
    const size_t at = src_.tiled ? size_t(TiledOffset(x, y, src_.width, 2))
                                 : (size_t(y) * (src_.pitch > src_.width ? src_.pitch : src_.width) + x) * 4;
    if (at + 4 > src_.size)
      return false;
    const uint8_t* texel = src_.base + at;
    const float* table = ByteToFloat();
    const float comps[6] = {table[texel[order_[0]]], table[texel[order_[1]]], table[texel[order_[2]]],
                            table[texel[order_[3]]], 0.0f, 1.0f};
    for (int i = 0; i < 4; ++i)
      out[i] = comps[swizzle_[i]];
    return true;
  }

 private:
  Source src_;
  uint8_t order_[4];
  uint8_t swizzle_[4];
};

struct Stats {
  uint64_t blocks = 0;
  uint64_t out_of_range = 0;  // source texels or destination blocks past their bytes
};

// Encodes rect row by row into the tiled destination, each block as eight big-endian words. Every
// block is written, zero blocks too: the destination can hold anything (a stale vertex buffer in
// the user's run), and an all-zero block is what the shader exports for an empty source.
inline Stats EncodeRect(const Source& src, const Dest& dst, const Rect& rect) {
  Stats stats;
  const SourceReader reader(src);
  for (uint32_t row = 0; row < rect.height; ++row) {
    for (uint32_t col = 0; col < rect.width; ++col) {
      float p[16][4];
      bool whole = true;
      for (uint32_t t = 0; t < 16; ++t) {
        const uint32_t x = (rect.src_x + col) * 4 + (t & 3), y = (rect.src_y + row) * 4 + (t >> 2);
        whole &= reader.Texel(x, y, p[t]);
      }
      const size_t at = TiledOffset(rect.dst_x + col, rect.dst_y + row, dst.width_blocks, 4);
      if (!whole || at + 16 > dst.size) {
        ++stats.out_of_range;
        continue;
      }
      uint16_t words[8];
      EncodeBlock(p, words);
      uint8_t* out = dst.base + at;
      for (int i = 0; i < 8; ++i) {
        out[2 * i] = uint8_t(words[i] >> 8);
        out[2 * i + 1] = uint8_t(words[i]);
      }
      ++stats.blocks;
    }
  }
  return stats;
}

}  // namespace ufc3_dxt5
