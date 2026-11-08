// AES-128-CBC and SHA-1 through Windows' own crypto (bcrypt.dll), so the shader cache tool carries
// no crypto code of its own and links nothing from the rexglue SDK. Errors throw std::runtime_error.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ufc3_shader_cache {

// Decrypts data in place: AES-128, CBC, zero IV, no padding (what the SDK's XexModule does).
// size must be a multiple of 16.
void Aes128CbcDecrypt(const uint8_t key[16], uint8_t* data, size_t size);

class Sha1 {
 public:
  Sha1();
  ~Sha1();
  Sha1(const Sha1&) = delete;
  Sha1& operator=(const Sha1&) = delete;

  void Update(const void* data, size_t size);
  std::array<uint8_t, 20> Finish();

 private:
  void* hash_ = nullptr;
};

// Lower-case hex of the first `chars` hex digits of a SHA-1 (Python's hexdigest()[:chars]).
std::string Sha1Hex(const void* data, size_t size, size_t chars = 40);
std::string ToHex(const uint8_t* data, size_t size);

}  // namespace ufc3_shader_cache
