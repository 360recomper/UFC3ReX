#include "xex_image.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

#include "win_crypto.h"

namespace ufc3_shader_cache {

namespace {

// Public retail XEX2 key, same constant as rexglue-sdk src/system/xex_module.cpp.
constexpr uint8_t kRetailKey[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};

constexpr uint32_t kHeaderFileFormatInfo = 0x3FF;
constexpr size_t kSecurityAesKey = 0x150;  // xex2_security_info::aes_key

class Reader {
 public:
  explicit Reader(std::string_view d) : d_(d) {}

  void Need(uint64_t off, uint64_t size) const {
    if (off > d_.size() || size > d_.size() - off)
      throw std::runtime_error("default.xex is cut short or damaged (header points past the end)");
  }
  uint32_t U32(uint64_t off) const {
    Need(off, 4);
    const auto* p = reinterpret_cast<const uint8_t*>(d_.data()) + off;
    return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3];
  }
  uint16_t U16(uint64_t off) const {
    Need(off, 2);
    const auto* p = reinterpret_cast<const uint8_t*>(d_.data()) + off;
    return static_cast<uint16_t>(p[0] << 8 | p[1]);
  }

 private:
  std::string_view d_;
};

}  // namespace

std::string LoadXexImage(std::string_view xex) {
  const Reader r(xex);
  r.Need(0, 24);
  if (xex.substr(0, 4) != "XEX2")
    throw std::runtime_error("default.xex is not an XEX2 file (wrong magic)");
  const uint32_t pe_offset = r.U32(8);
  const uint32_t security_offset = r.U32(16);
  const uint32_t header_count = r.U32(20);

  uint64_t format_info = 0;
  for (uint32_t i = 0; i < header_count; ++i) {
    if (r.U32(24 + uint64_t{i} * 8) == kHeaderFileFormatInfo)
      format_info = r.U32(24 + uint64_t{i} * 8 + 4);
  }
  if (!format_info) throw std::runtime_error("default.xex has no file format header");

  const uint32_t info_size = r.U32(format_info);
  const uint16_t encryption = r.U16(format_info + 4);
  const uint16_t compression = r.U16(format_info + 6);
  if (compression != 1) {
    throw std::runtime_error("default.xex uses compression type " + std::to_string(compression) +
                             "; only basic (1) is supported");
  }
  if (encryption > 1) {
    throw std::runtime_error("default.xex uses unknown encryption type " +
                             std::to_string(encryption));
  }
  if (info_size < 8) throw std::runtime_error("default.xex has a bad file format header");

  std::vector<std::pair<uint32_t, uint32_t>> blocks;  // data size, zero size
  uint64_t data_size = 0, image_size = 0;
  for (uint32_t i = 0; i < (info_size - 8) / 8; ++i) {
    const uint32_t data = r.U32(format_info + 8 + uint64_t{i} * 8);
    const uint32_t zero = r.U32(format_info + 8 + uint64_t{i} * 8 + 4);
    blocks.emplace_back(data, zero);
    data_size += data;
    image_size += uint64_t{data} + zero;
  }
  r.Need(pe_offset, data_size);
  if (image_size > (uint64_t{1} << 30)) throw std::runtime_error("default.xex image is too large");

  std::string data(xex.substr(pe_offset, data_size));
  if (encryption == 1) {
    r.Need(uint64_t{security_offset} + kSecurityAesKey, 16);
    uint8_t session_key[16];
    std::memcpy(session_key, xex.data() + security_offset + kSecurityAesKey, 16);
    Aes128CbcDecrypt(kRetailKey, session_key, 16);
    if (data.size() % 16 != 0)
      throw std::runtime_error("default.xex image data is not a whole number of AES blocks");
    // One chain across all blocks.
    Aes128CbcDecrypt(session_key, reinterpret_cast<uint8_t*>(data.data()), data.size());
  }

  std::string image;
  image.reserve(image_size);
  size_t pos = 0;
  for (const auto& [data_bytes, zero_bytes] : blocks) {
    image.append(data, pos, data_bytes);
    image.append(zero_bytes, '\0');
    pos += data_bytes;
  }
  if (image.size() < 2 || image[0] != 'M' || image[1] != 'Z') {
    throw std::runtime_error(
        "default.xex did not decrypt to a program image (no MZ header); is it a retail disc file?");
  }
  return image;
}

}  // namespace ufc3_shader_cache
