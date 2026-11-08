#include "shader_set.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

#define MINIZ_HEADER_FILE_ONLY
#include "miniz.h"

#include "blood_light_patterns.h"
#include "bloom_patterns.h"
#include "dof_patterns.h"
#include "win_crypto.h"
#include "xex_image.h"

namespace ufc3_shader_cache {

namespace {

constexpr size_t kContainerHeader = 36;
constexpr size_t kMaxInflate = size_t{64} << 20;  // nr_shader_extract.py's cap per blob
constexpr int kMaxDepth = 4;                      // YZLI blobs inside YZLI blobs
constexpr size_t kZlibHeaderSkip = 16;            // "YZLI" + sizes, then the zlib stream

uint32_t Be32(const char* p) {
  const auto* u = reinterpret_cast<const uint8_t*>(p);
  return uint32_t{u[0]} << 24 | uint32_t{u[1]} << 16 | uint32_t{u[2]} << 8 | u[3];
}

std::string Hex(size_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "%zx", value);
  return text;
}

// Every 4-aligned shader container in d: fn(offset, bytes). A container starts 10 2A 11 xx, has
// two zero dwords at +0x1C, and its virtual + physical size fits in what is left of d.
template <typename Fn>
void ForEachContainer(std::string_view d, Fn&& fn) {
  static constexpr std::string_view kMagic("\x10\x2a\x11", 3);
  size_t i = 0;
  for (;;) {
    i = d.find(kMagic, i);
    if (i == std::string_view::npos || i + kContainerHeader > d.size()) return;
    if (i % 4 == 0) {
      const char* p = d.data() + i;
      const uint64_t total = uint64_t{Be32(p + 4)} + Be32(p + 8);
      if (Be32(p + 28) == 0 && Be32(p + 32) == 0 && total > kContainerHeader &&
          total <= d.size() - i) {
        fn(i, d.substr(i, static_cast<size_t>(total)));
        i += static_cast<size_t>(total);
        continue;
      }
    }
    ++i;
  }
}

// What Python's zlib.decompressobj().decompress(in, 64 MiB) returns, with "" for a stream zlib
// rejects: the output of a complete stream (bytes after it are ignored), what could be decoded of
// a stream the input cuts short, and never more than the cap.
//
// miniz decodes into one linear buffer (grown as needed), not a 32 KB ring: only then does it
// reject a match that reaches back before the start of the output, as zlib does. In that mode it
// skips the header's window-size check, so that is done here. On good streams the two libraries
// agree; on damaged ones miniz is still a little more forgiving than zlib (it has no check for a
// missing end-of-block code or for the reserved length / distance symbols).
std::string InflateZlibStream(std::string_view in) {
  if (in.empty() || (static_cast<uint8_t>(in[0]) >> 4) > 7) return {};  // window over 32 KB
  tinfl_decompressor decomp;
  tinfl_init(&decomp);
  std::string out(size_t{64} << 10, '\0');
  size_t in_pos = 0, out_pos = 0;
  for (;;) {
    size_t in_n = in.size() - in_pos;
    size_t out_n = out.size() - out_pos;
    // HAS_MORE_INPUT: at the end of the input miniz then stops instead of decoding zeros.
    const tinfl_status status = tinfl_decompress(
        &decomp, reinterpret_cast<const mz_uint8*>(in.data()) + in_pos, &in_n,
        reinterpret_cast<mz_uint8*>(out.data()), reinterpret_cast<mz_uint8*>(out.data()) + out_pos,
        &out_n,
        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT |
            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    in_pos += in_n;
    out_pos += out_n;
    if (out_pos >= kMaxInflate) break;  // Python stops reading here, before any later error
    if (status < 0) return {};
    if (status == TINFL_STATUS_DONE || status == TINFL_STATUS_NEEDS_MORE_INPUT) break;
    out.resize(std::min(out.size() * 2, kMaxInflate));  // TINFL_STATUS_HAS_MORE_OUTPUT
  }
  out.resize(out_pos);
  return out;
}

class Finder {
 public:
  struct Entry {
    const std::string* data;  // key of index_, stable
    std::string source;
  };

  void Walk(std::string_view d, const std::string& source, int depth = 0) {
    ForEachContainer(d, [&](size_t offset, std::string_view container) {
      auto [it, added] = index_.try_emplace(std::string(container), entries_.size());
      if (added) entries_.push_back({&it->first, source + "@" + Hex(offset)});
    });
    if (depth >= kMaxDepth) return;
    size_t j = 0;
    while ((j = d.find("YZLI", j)) != std::string_view::npos) {
      if (j + kZlibHeaderSkip < d.size()) {
        const std::string sub = InflateZlibStream(d.substr(j + kZlibHeaderSkip));
        if (!sub.empty()) {
          ++zlib_blobs;
          zlib_bytes += sub.size();
          Walk(sub, source + "@yz" + Hex(j), depth + 1);
        }
      }
      j += 4;
    }
  }

  const std::vector<Entry>& entries() const { return entries_; }
  size_t zlib_blobs = 0;
  uint64_t zlib_bytes = 0;

 private:
  std::unordered_map<std::string, size_t> index_;
  std::vector<Entry> entries_;  // first-seen order
};

struct PatchTarget {
  const char* feature;
  std::string_view original;  // microcode to look for
  size_t offset;              // where the patch goes inside it
  std::string_view patch;
};

std::string_view View(const uint8_t* data, size_t size) {
  return {reinterpret_cast<const char*>(data), size};
}

// The microcode patches the bloom and depth-of-field switches make in guest memory
// (src/no_bloom.h, src/no_dof.h), in the order nr_shader_extract.py applies them.
std::vector<PatchTarget> PatchTargets() {
  std::vector<PatchTarget> targets;
  targets.push_back({"nobloom", View(ufc3_no_bloom::kOrig, sizeof(ufc3_no_bloom::kOrig)),
                     ufc3_no_bloom::kPatchOffset,
                     View(ufc3_no_bloom::kPatch, sizeof(ufc3_no_bloom::kPatch))});
  for (const auto& s : ufc3_no_dof::kShaders)
    targets.push_back({"nodof", View(s.ucode, s.len), s.off, View(s.patch, 4)});
  // Blood light fix (src/blood_light.h, on by default): the two fighter skin pixel shaders.
  targets.push_back({"bloodlight", View(ufc3_blood_light::kBodyOrig, sizeof(ufc3_blood_light::kBodyOrig)),
                     ufc3_blood_light::kBodyOff,
                     View(ufc3_blood_light::kBodyPatch, sizeof(ufc3_blood_light::kBodyPatch))});
  targets.push_back({"bloodlight", View(ufc3_blood_light::kFaceOrig, sizeof(ufc3_blood_light::kFaceOrig)),
                     ufc3_blood_light::kFaceOff,
                     View(ufc3_blood_light::kFacePatch, sizeof(ufc3_blood_light::kFacePatch))});
  for (const auto& t : targets) {
    if (t.offset + t.patch.size() > t.original.size())
      throw std::runtime_error("internal: a microcode patch does not fit its pattern");
  }
  return targets;
}

}  // namespace

std::string PathText(const std::filesystem::path& path) {
  const auto u8 = path.u8string();
  return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

std::string ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw std::runtime_error("cannot open " + PathText(path));
  const std::streamoff size = f.tellg();
  if (size < 0) throw std::runtime_error("cannot read " + PathText(path));
  std::string data(static_cast<size_t>(size), '\0');
  f.seekg(0);
  f.read(data.data(), size);
  if (f.gcount() != size) throw std::runtime_error("cannot read " + PathText(path));
  return data;
}

void WriteFileBytes(const std::filesystem::path& path, std::string_view data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  f.close();
  if (!f) throw std::runtime_error("cannot write " + PathText(path));
}

ShaderSet ExtractShaderSet(const std::filesystem::path& game_root) {
  const auto pac_path = game_root / "DAT" / "shader" / "fxl.pac";
  const auto xex_path = game_root / "default.xex";
  std::error_code ec;
  for (const auto& p : {pac_path, xex_path}) {
    if (!std::filesystem::is_regular_file(p, ec)) {
      throw std::runtime_error(PathText(p) + " not found: the game data folder must hold " +
                               "default.xex and DAT\\shader\\fxl.pac");
    }
  }

  ShaderSet set;
  Finder finder;
  finder.Walk(ReadFileBytes(pac_path), "fxl.pac");
  set.from_pac = finder.entries().size();
  finder.Walk(LoadXexImage(ReadFileBytes(xex_path)), "default.xex");
  set.containers = finder.entries().size();
  set.from_xex = set.containers - set.from_pac;
  set.zlib_blobs = finder.zlib_blobs;
  set.zlib_bytes = finder.zlib_bytes;
  if (set.from_pac == 0)
    throw std::runtime_error("no shaders found in " + PathText(pac_path));

  // One patched copy per (container, feature); several patches of one feature stack up in it.
  struct Variant {
    size_t entry;
    const char* feature;
    std::string data;
  };
  std::vector<Variant> variants;
  for (const auto& target : PatchTargets()) {
    for (size_t e = 0; e < finder.entries().size(); ++e) {
      const std::string& container = *finder.entries()[e].data;
      for (size_t p = container.find(target.original); p != std::string::npos;
           p = container.find(target.original, p + 1)) {
        auto it = std::find_if(variants.begin(), variants.end(), [&](const Variant& v) {
          return v.entry == e && std::strcmp(v.feature, target.feature) == 0;
        });
        if (it == variants.end())
          it = variants.insert(it, Variant{e, target.feature, container});
        it->data.replace(p + target.offset, target.patch.size(), target.patch);
        ++set.patch_hits[target.feature];
      }
    }
  }
  set.variants = variants.size();

  std::vector<std::string> stems(finder.entries().size());
  for (size_t e = 0; e < finder.entries().size(); ++e) {
    const auto& entry = finder.entries()[e];
    stems[e] = Sha1Hex(entry.data->data(), entry.data->size(), 16);
    set.files.push_back({stems[e] + ".bin", *entry.data, entry.source});
  }
  for (auto& v : variants) {
    set.files.push_back({stems[v.entry] + "." + v.feature + ".bin", std::move(v.data),
                         finder.entries()[v.entry].source + " +" + v.feature});
  }
  std::sort(set.files.begin(), set.files.end(),
            [](const ShaderFile& a, const ShaderFile& b) { return a.name < b.name; });

  Sha1 total;
  for (const auto& f : set.files) {
    total.Update(f.name.data(), f.name.size());
    total.Update(f.data.data(), f.data.size());
  }
  const auto digest = total.Finish();
  set.set_hash = ToHex(digest.data(), digest.size()).substr(0, 16);
  return set;
}

void WriteShaderSet(const ShaderSet& set, const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (!std::filesystem::is_directory(dir, ec))
    throw std::runtime_error("cannot create folder " + PathText(dir));
  std::string sources;
  for (const auto& f : set.files) {
    WriteFileBytes(dir / f.name, f.data);
    // CRLF: the Python tool writes this list in text mode, and the two outputs are diffed.
    sources += f.name + " " + f.source + "\r\n";
  }
  WriteFileBytes(dir / "_sources.txt", sources);
}

}  // namespace ufc3_shader_cache
