#include "win_crypto.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include <windows.h>

#include <bcrypt.h>

namespace ufc3_shader_cache {

namespace {

void Check(NTSTATUS status, const char* what) {
  if (status < 0) {
    char text[96];
    std::snprintf(text, sizeof(text), "%s failed (NTSTATUS 0x%08lX)", what,
                  static_cast<unsigned long>(status));
    throw std::runtime_error(text);
  }
}

BCRYPT_ALG_HANDLE Sha1Provider() {
  static BCRYPT_ALG_HANDLE provider = [] {
    BCRYPT_ALG_HANDLE h = nullptr;
    Check(BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA1_ALGORITHM, nullptr, 0), "opening SHA-1");
    return h;
  }();
  return provider;
}

}  // namespace

void Aes128CbcDecrypt(const uint8_t key[16], uint8_t* data, size_t size) {
  if (size % 16 != 0) throw std::runtime_error("AES data size is not a multiple of 16");
  BCRYPT_ALG_HANDLE provider = nullptr;
  Check(BCryptOpenAlgorithmProvider(&provider, BCRYPT_AES_ALGORITHM, nullptr, 0), "opening AES");
  BCRYPT_KEY_HANDLE key_handle = nullptr;
  NTSTATUS status = BCryptSetProperty(
      provider, BCRYPT_CHAINING_MODE,
      reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
      sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
  if (status >= 0) {
    status = BCryptGenerateSymmetricKey(provider, &key_handle, nullptr, 0,
                                        const_cast<PUCHAR>(key), 16, 0);
  }
  // The IV is updated by each call, so the chain carries across the 1 GB slices.
  uint8_t iv[16] = {};
  size_t done = 0;
  while (status >= 0 && done < size) {
    const ULONG part = static_cast<ULONG>(std::min<size_t>(size - done, size_t{1} << 30));
    ULONG written = 0;
    status = BCryptDecrypt(key_handle, data + done, part, nullptr, iv, sizeof(iv), data + done,
                           part, &written, 0);
    if (status >= 0 && written != part) status = static_cast<NTSTATUS>(0xC0000001L);
    done += part;
  }
  if (key_handle) BCryptDestroyKey(key_handle);
  BCryptCloseAlgorithmProvider(provider, 0);
  Check(status, "AES decryption");
}

Sha1::Sha1() {
  BCRYPT_HASH_HANDLE h = nullptr;
  Check(BCryptCreateHash(Sha1Provider(), &h, nullptr, 0, nullptr, 0, 0), "creating a SHA-1 hash");
  hash_ = h;
}

Sha1::~Sha1() {
  if (hash_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
}

void Sha1::Update(const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  while (size) {
    const ULONG part = static_cast<ULONG>(std::min<size_t>(size, size_t{1} << 30));
    Check(BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash_), const_cast<PUCHAR>(p), part, 0),
          "SHA-1 update");
    p += part;
    size -= part;
  }
}

std::array<uint8_t, 20> Sha1::Finish() {
  std::array<uint8_t, 20> digest{};
  Check(BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), digest.data(),
                         static_cast<ULONG>(digest.size()), 0),
        "SHA-1 finish");
  return digest;
}

std::string ToHex(const uint8_t* data, size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 15]);
  }
  return out;
}

std::string Sha1Hex(const void* data, size_t size, size_t chars) {
  Sha1 sha;
  sha.Update(data, size);
  const auto digest = sha.Finish();
  return ToHex(digest.data(), digest.size()).substr(0, chars);
}

}  // namespace ufc3_shader_cache
