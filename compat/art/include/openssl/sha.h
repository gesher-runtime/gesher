// Gesher: the part of BoringSSL's <openssl/sha.h> dex2oat uses (the oat
// file's build ID), implemented over Windows' BCrypt (compat/art/sha1_bcrypt.cc).
#ifndef GESHER_OPENSSL_SHA_H_
#define GESHER_OPENSSL_SHA_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA_DIGEST_LENGTH 20

typedef struct sha_state_st {
  void* hash;  // BCRYPT_HASH_HANDLE
} SHA_CTX;

int SHA1_Init(SHA_CTX* sha);
int SHA1_Update(SHA_CTX* sha, const void* data, size_t len);
int SHA1_Final(uint8_t out[SHA_DIGEST_LENGTH], SHA_CTX* sha);

#ifdef __cplusplus
}
#endif

#endif  // GESHER_OPENSSL_SHA_H_
