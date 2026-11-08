// Loads a retail XEX2 (default.xex) to its in-memory PE image the way the SDK's XexModule does:
// session key from the retail key, AES-128-CBC over the image data, then the basic-compression
// block list (data + zero runs). Port of tools/xex_image.py. Errors throw std::runtime_error.

#pragma once

#include <string>
#include <string_view>

namespace ufc3_shader_cache {

// `xex` is the whole file. Returns the PE image ("MZ...").
std::string LoadXexImage(std::string_view xex);

}  // namespace ufc3_shader_cache
