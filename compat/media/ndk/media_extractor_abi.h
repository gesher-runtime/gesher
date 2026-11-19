// Android's LP64 extractor and data-source value types, pinned to API 36.
// In particular the callbacks arrive from guest ELF and must be called
// with the guest ABI, including the reverse call on Windows x86-64.
#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>

#if defined(__x86_64__)
#define GESHER_MEDIA_GUEST_ABI __attribute__((sysv_abi))
#elif defined(__aarch64__)
#define GESHER_MEDIA_GUEST_ABI
#endif

namespace gesher::media::extractor_abi {

using ReadAt = ssize_t (GESHER_MEDIA_GUEST_ABI *)(void*, int64_t, void*, size_t);
using GetSize = ssize_t (GESHER_MEDIA_GUEST_ABI *)(void*);
using Close = void (GESHER_MEDIA_GUEST_ABI *)(void*);
using GetAvailableSize = ssize_t (GESHER_MEDIA_GUEST_ABI *)(void*, int64_t);

struct PsshEntry {
  uint8_t uuid[16];
  size_t datalen;
  void* data;
};

struct PsshInfo {
  size_t numentries;
  PsshEntry entries[0];
};

struct CryptoPattern {
  int32_t encryptBlocks;
  int32_t skipBlocks;
};

static_assert(sizeof(ssize_t) == 8);
static_assert(sizeof(PsshEntry) == 32);
static_assert(offsetof(PsshInfo, entries) == 8);
static_assert(offsetof(PsshEntry, datalen) == 16 && offsetof(PsshEntry, data) == 24);
static_assert(sizeof(CryptoPattern) == 8);

}  // namespace gesher::media::extractor_abi
