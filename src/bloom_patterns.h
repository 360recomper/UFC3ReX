// Microcode pattern of the bloom bright-pass pixel shader and its patch (what they do: no_bloom.h).
//
// Plain data with no SDK headers, like dof_patterns.h: the shader cache tool
// (tools/native/shader_extract) includes this file to build the patched cache variant, and
// tools/nr_shader_extract.py parses it. Keep the names kOrig / kPatch / kPatchOffset.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ufc3_no_bloom {

// Last two ALU instructions of the bright-pass shader (big-endian dwords) and
// the replacement for the last one: ADDv oC0 = r0 + (-r0) instead of MULv.
inline constexpr uint8_t kOrig[] = {
    0x4c, 0x1e, 0x00, 0x00, 0x00, 0xfc, 0xb1, 0x6c, 0xe1, 0x01, 0x00, 0x00,  // ALU3
    0xc8, 0x8f, 0xc0, 0x00, 0x00, 0x15, 0x6c, 0x00, 0xe1, 0x00, 0x00, 0x00,  // ALU4
};
inline constexpr uint8_t kPatch[] = {
    0xc8, 0x8f, 0xc0, 0x00, 0x04, 0x6c, 0x6c, 0x00, 0xe0, 0x00, 0x00, 0x00,
};
// Where kPatch goes inside kOrig.
inline constexpr size_t kPatchOffset = 12;

}  // namespace ufc3_no_bloom
