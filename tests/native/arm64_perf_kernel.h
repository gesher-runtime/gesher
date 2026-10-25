// A CPU-bound kernel compiled twice: into an arm64-v8a .so (run through the
// translator) and natively into arm64_elf_test.exe, so the translation
// overhead on real compiled code can be measured (docs/arm64.md).
// Integer arithmetic, branches, a table in memory and a byte loop: a sieve of
// Eratosthenes, a table-driven CRC-32 and an LCG-mixed hash.
#pragma once

#include <stdint.h>

static inline uint64_t gesher_perf_kernel_impl(uint32_t rounds) {
  static uint8_t sieve[65536];
  static uint32_t crc_table[256];
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[i] = c;
  }
  uint64_t acc = 0;
  for (uint32_t r = 0; r < rounds; ++r) {
    // Sieve.
    for (uint32_t i = 0; i < sizeof(sieve); ++i) sieve[i] = 1;
    sieve[0] = sieve[1] = 0;
    for (uint32_t i = 2; i * i < sizeof(sieve); ++i) {
      if (!sieve[i]) continue;
      for (uint32_t j = i * i; j < sizeof(sieve); j += i) sieve[j] = 0;
    }
    uint32_t primes = 0;
    for (uint32_t i = 0; i < sizeof(sieve); ++i) primes += sieve[i];
    // CRC-32 over the sieve.
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < sizeof(sieve); ++i) {
      crc = crc_table[(crc ^ (sieve[i] + r)) & 0xFF] ^ (crc >> 8);
    }
    // LCG mix.
    uint64_t x = acc ^ r;
    for (uint32_t i = 0; i < 100000; ++i) x = x * 6364136223846793005ull + 1442695040888963407ull;
    acc += primes + (crc ^ 0xFFFFFFFFu) + (x >> 33);
  }
  return acc;
}
