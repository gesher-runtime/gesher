// Gesher: SHA-1 for dex2oat's build IDs (see include/openssl/sha.h).

#include <openssl/sha.h>

#include <windows.h>
#include <bcrypt.h>

#ifndef BCRYPT_SHA1_ALG_HANDLE  // Windows 10's pseudo-handle, if the SDK lacks it
#define BCRYPT_SHA1_ALG_HANDLE ((BCRYPT_ALG_HANDLE)0x00000031)
#endif

extern "C" {

int SHA1_Init(SHA_CTX* sha) {
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptCreateHash(BCRYPT_SHA1_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
    sha->hash = nullptr;
    return 0;
  }
  sha->hash = hash;
  return 1;
}

int SHA1_Update(SHA_CTX* sha, const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  while (len > 0) {
    ULONG n = len > 0x40000000u ? 0x40000000u : static_cast<ULONG>(len);
    if (BCryptHashData(sha->hash, const_cast<PUCHAR>(p), n, 0) != 0) return 0;
    p += n;
    len -= n;
  }
  return 1;
}

int SHA1_Final(uint8_t out[SHA_DIGEST_LENGTH], SHA_CTX* sha) {
  NTSTATUS status = BCryptFinishHash(sha->hash, out, SHA_DIGEST_LENGTH, 0);
  BCryptDestroyHash(sha->hash);
  sha->hash = nullptr;
  return status == 0;
}

}  // extern "C"
