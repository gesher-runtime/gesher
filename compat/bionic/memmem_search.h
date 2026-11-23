// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Allocation-free binary substring search for the Bionic memmem entry point.
//
// This is an independently written implementation of Crochemore and Perrin's
// Two-Way algorithm: find a critical factorization, compare its right half
// first, then use the factor's period to avoid rechecking a periodic prefix.
// Long needles take O(haystack_size + needle_size) time and constant storage.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gesher::bionic::search {

struct ByteSearchFactor {
  size_t split;
  size_t period;
};

inline ByteSearchFactor MaximalSuffix(const uint8_t* needle, size_t size, bool reverse) {
  // split is one past the maximal suffix's starting index in the usual
  // formulation. Zero represents its initial -1 sentinel without unsigned
  // wraparound; candidate + matched always remains inside the needle.
  size_t split = 0, candidate = 0, matched = 1, period = 1;
  while (matched < size - candidate) {
    const uint8_t a = needle[candidate + matched];
    const uint8_t b = needle[split + matched - 1];
    if (a == b) {
      if (matched == period) {
        candidate += period;
        matched = 1;
      } else {
        ++matched;
      }
    } else if ((a < b) != reverse) {
      candidate += matched;
      matched = 1;
      period = candidate - split + 1;
    } else {
      split = candidate + 1;
      candidate = split;
      matched = period = 1;
    }
  }
  return {split, period};
}

inline const void* FindBytes(const void* haystack_data, size_t haystack_size,
                            const void* needle_data, size_t needle_size) {
  if (needle_size == 0) return haystack_data;
  if (needle_size > haystack_size) return nullptr;
  const auto* haystack = static_cast<const uint8_t*>(haystack_data);
  const auto* needle = static_cast<const uint8_t*>(needle_data);
  if (needle_size == 1) return std::memchr(haystack, *needle, haystack_size);

  size_t remaining = haystack_size - needle_size;
  if (remaining == 0) return std::memcmp(haystack, needle, needle_size) == 0 ? haystack : nullptr;
  // Immediate matches need no factorization or first-byte search.
  if (*haystack == *needle && std::memcmp(haystack, needle, needle_size) == 0) return haystack;
  ++haystack;
  --remaining;

  // Tiny needles do not amortize factorization. Each candidate compares at
  // most 16 bytes, so this path also has a fixed linear work bound. At most
  // four remaining candidates similarly need at most four full comparisons.
  // memchr skips impossible starts using the host's optimized routine.
  if (needle_size <= 16 || remaining < 4) {
    for (;;) {
      const auto* found = static_cast<const uint8_t*>(
          std::memchr(haystack, *needle, remaining + 1));
      if (found == nullptr) return nullptr;
      if (std::memcmp(found, needle, needle_size) == 0) return found;
      const size_t advance = static_cast<size_t>(found - haystack) + 1;
      if (advance > remaining) return nullptr;
      haystack += advance;
      remaining -= advance;
    }
  }

  // A haystack without another first-byte candidate needs only this scan.
  const auto* first = static_cast<const uint8_t*>(
      std::memchr(haystack, *needle, remaining + 1));
  if (first == nullptr) return nullptr;
  remaining -= static_cast<size_t>(first - haystack);
  haystack = first;
  const ByteSearchFactor forward = MaximalSuffix(needle, needle_size, false);
  const ByteSearchFactor backward = MaximalSuffix(needle, needle_size, true);
  const ByteSearchFactor factor = forward.split >= backward.split ? forward : backward;
  const size_t split = factor.split;
  size_t period = factor.period;
  size_t retained = 0;
  if (period <= needle_size - split &&
      std::memcmp(needle, needle + period, split) == 0) {
    retained = needle_size - period;
  } else {
    // A nonperiodic needle can advance past the longer factor after its
    // right half matches. split is positive here (a zero split is periodic).
    period = split > needle_size - split ? split : needle_size - split + 1;
  }

  size_t known = 0;
  for (;;) {
    size_t right = split > known ? split : known;
    while (right < needle_size && needle[right] == haystack[right]) ++right;
    size_t advance;
    if (right < needle_size) {
      advance = right - split + 1;
      known = 0;
    } else {
      size_t left = split;
      while (left > known && needle[left - 1] == haystack[left - 1]) --left;
      if (left <= known) return haystack;
      advance = period;
      known = retained;
    }
    // Subtraction-based bounds avoid overflow even for large supplied sizes.
    if (advance > remaining) return nullptr;
    haystack += advance;
    remaining -= advance;
  }
}

}  // namespace gesher::bionic::search
