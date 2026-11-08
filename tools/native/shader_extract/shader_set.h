// The game's shader containers, pulled from the player's own disc files. Port of
// tools/nr_shader_extract.py: the two must give the same files (same names, same bytes, same set
// hash), because the shader cache built from them is keyed by container hash.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ufc3_shader_cache {

struct ShaderFile {
  std::string name;    // <sha1[:16]>.bin, or <sha1[:16]>.<feature>.bin for a patched variant
  std::string data;    // the XDK shader container
  std::string source;  // where it was first seen, e.g. fxl.pac@yz1a40@2c
};

struct ShaderSet {
  std::vector<ShaderFile> files;  // sorted by name
  size_t containers = 0;          // unique, without the patched variants
  size_t from_pac = 0;
  size_t from_xex = 0;
  size_t variants = 0;
  std::map<std::string, size_t> patch_hits;  // feature -> patched sites
  size_t zlib_blobs = 0;                     // YZLI blobs unpacked on the way
  uint64_t zlib_bytes = 0;
  std::string set_hash;  // sha1 over name + data of every file, first 16 hex digits
};

// game_root holds default.xex and DAT/shader/fxl.pac. Throws std::runtime_error.
ShaderSet ExtractShaderSet(const std::filesystem::path& game_root);

// Writes every file plus _sources.txt into dir (created if missing).
void WriteShaderSet(const ShaderSet& set, const std::filesystem::path& dir);

std::string ReadFileBytes(const std::filesystem::path& path);
void WriteFileBytes(const std::filesystem::path& path, std::string_view data);
std::string PathText(const std::filesystem::path& path);  // UTF-8, for messages

}  // namespace ufc3_shader_cache
