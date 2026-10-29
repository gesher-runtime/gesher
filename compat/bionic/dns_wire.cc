// DNS name decoding, based on ISC's ns_name.c in bionic android-16.0.0_r4.
// Copyright (c) 2004 Internet Systems Consortium, Inc. ("ISC")
// Copyright (c) 1996,1999 Internet Software Consortium.
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS. IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING
// FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
// NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH
// THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "shim_internal.h"
#include <cerrno>
#include <cstdio>

namespace gesher::bionic {
namespace {
constexpr size_t kMaxName = 255;
int BadName() { errno = 90; return -1; } // bionic EMSGSIZE

// The old bitstring label remains supported, as in the pinned bionic source.
int LabelBytes(const unsigned char* p) {
  if (*p < 64) return *p;
  if (*p == 0x41) return ((p[1] ? p[1] : 256) + 7) / 8 + 1;
  return -1;
}

int NameUnpack(const unsigned char* msg, const unsigned char* eom,
               const unsigned char* src, unsigned char* dst, size_t size) {
  if (src < msg || src >= eom || size == 0) return BadName();
  const size_t length = static_cast<size_t>(eom - msg);
  size_t at = static_cast<size_t>(src - msg), out = 0, checked = 0;
  int consumed = -1;
  for (;;) {
    if (at >= length) return BadName();
    unsigned n = msg[at++];
    if (!n) {
      if (out >= size) return BadName();
      dst[out] = 0;
      return consumed < 0 ? static_cast<int>(msg + at - src) : consumed;
    }
    if ((n & 0xc0) == 0xc0) {
      if (at >= length) return BadName();
      size_t target = ((n & 63) << 8) | msg[at];
      if (target >= length) return BadName();
      if (consumed < 0) consumed = static_cast<int>(msg + at + 1 - src);
      at = target;
      checked += 2;
      if (checked >= length) return BadName();
      continue;
    }
    // LabelBytes reads a second byte only for bitstring labels.
    if (n == 0x41 && at >= length) return BadName();
    int count = LabelBytes(msg + at - 1);
    if (count < 0) return BadName();
    size_t bytes = static_cast<size_t>(count);
    if (bytes >= length - at || out >= size || bytes + 1 >= size - out) return BadName();
    dst[out++] = static_cast<unsigned char>(n);
    std::memcpy(dst + out, msg + at, bytes);
    out += bytes;
    at += bytes;
    checked += bytes + 1;
  }
}

int NameNtop(const unsigned char* src, char* dst, size_t size, bool unsigned_char = false) {
  size_t out = 0;
  auto emit = [&](char c) { if (out >= size) return false; dst[out++] = c; return true; };
  const unsigned char* p = src;
  while (*p) {
    unsigned n = *p;
    int count = LabelBytes(p++);
    if (count < 0) return BadName();
    if (out && !emit('.')) return BadName();
    if (n == 0x41) {
      unsigned bits = *p ? *p : 256;
      ++p;
      // Preserve the pinned decode_bitstring headroom rule, including its
      // conservative extra byte beyond the rendered name and terminator.
      size_t required = (bits + 3) / 4 + sizeof("\\[x/]") +
                        (bits > 99 ? 3 : bits > 9 ? 2 : 1);
      if (out >= size || required >= size - out) return BadName();
      char text[80];
      size_t used = 0;
      text[used++] = '\\'; text[used++] = '['; text[used++] = 'x';
      constexpr char hex[] = "0123456789abcdef";
      unsigned full = bits / 8 * 8;
      for (unsigned b = 0; b < full; b += 4) {
        unsigned nibble = (p[b / 8] >> (4 - (b % 8))) & 15;
        text[used++] = hex[nibble];
      }
      unsigned remaining = bits - full;
      if (remaining > 4) {
        // Upstream's plain char tc promotes differently on Android x86-64
        // (signed) and arm64 (unsigned). Retain even the historical extra
        // hex digit produced by a negative partial byte on x86-64.
        int tc = unsigned_char ? p[full / 8] : static_cast<int8_t>(p[full / 8]);
        unsigned value = static_cast<unsigned>(tc & (0xff << (8 - remaining)));
        int digits = std::snprintf(text + used, sizeof(text) - used, "%02x", value);
        if (digits < 0) return BadName();
        used += static_cast<size_t>(digits);
      } else if (remaining) {
        unsigned nibble = (p[full / 8] >> 4) & (15 << (4 - remaining));
        text[used++] = hex[nibble];
      }
      int written = std::snprintf(text + used, sizeof(text) - used, "/%u]", bits);
      if (written < 0) return BadName();
      used += static_cast<size_t>(written);
      for (size_t i = 0; i < used; ++i) if (!emit(text[i])) return BadName();
      p += (bits + 7) / 8;
      continue;
    }
    for (int i = 0; i < count; ++i) {
      unsigned char c = *p++;
      if (std::strchr("\".;\\()@$", c) && c != 0) {
        if (!emit('\\') || !emit(static_cast<char>(c))) return BadName();
      } else if (c <= 0x20 || c >= 0x7f) {
        if (!emit('\\') || !emit('0' + c / 100) || !emit('0' + (c % 100) / 10) ||
            !emit('0' + c % 10)) return BadName();
      } else if (!emit(static_cast<char>(c))) return BadName();
    }
  }
  if (!out && !emit('.')) return BadName();
  if (!emit('\0')) return BadName();
  return static_cast<int>(out);
}

int NameUncompress(const unsigned char* msg, const unsigned char* eom,
                   const unsigned char* src, char* dst, size_t size, bool unsigned_char = false) {
  unsigned char wire[kMaxName];
  int consumed = NameUnpack(msg, eom, src, wire, sizeof(wire));
  if (consumed < 0 || NameNtop(wire, dst, size, unsigned_char) < 0) return -1;
  return consumed;
}
int NameSkip(const unsigned char** cursor, const unsigned char* eom) {
  const unsigned char* p = *cursor;
  while (p < eom) {
    unsigned n = *p++;
    if (!n) { *cursor = p; return 0; }
    if ((n & 0xc0) == 0xc0) {
      if (p >= eom) return BadName();
      *cursor = p + 1; return 0;
    }
    if (n == 0x41 && p >= eom) return BadName();
    int count = LabelBytes(p - 1);
    if (count < 0 || static_cast<size_t>(count) >= static_cast<size_t>(eom - p))
      return BadName();
    p += count;
  }
  return BadName();
}

int BIONIC_SYSV Unpack(const unsigned char* m, const unsigned char* e, const unsigned char* s,
                    unsigned char* d, size_t z) { return NameUnpack(m,e,s,d,z); }
int BIONIC_SYSV Ntop(const unsigned char* s, char* d, size_t z) { return NameNtop(s,d,z); }
int BIONIC_SYSV Uncompress(const unsigned char* m, const unsigned char* e, const unsigned char* s,
                        char* d, size_t z) { return NameUncompress(m,e,s,d,z); }
int BIONIC_SYSV Expand(const unsigned char* m, const unsigned char* e, const unsigned char* s,
                    char* d, int z, bool unsigned_char = false) {
  if (z < 0) return BadName();
  int result = NameUncompress(m,e,s,d,static_cast<size_t>(z),unsigned_char);
  if (result >= 0 && d[0] == '.') d[0] = '\0'; // historical dn_expand root convention
  return result;
}
// Private host entries selected by the ARM64 guest-symbol overrides. Their
// public wrappers keep the exact five/three-argument System V signatures.
int BIONIC_SYSV ExpandX86(const unsigned char* m, const unsigned char* e, const unsigned char* s,
                        char* d, int z) { return Expand(m,e,s,d,z); }
int BIONIC_SYSV NtopArm64(const unsigned char* s, char* d, size_t z) { return NameNtop(s,d,z,true); }
int BIONIC_SYSV UncompressArm64(const unsigned char* m, const unsigned char* e, const unsigned char* s,
                              char* d, size_t z) { return NameUncompress(m,e,s,d,z,true); }
int BIONIC_SYSV ExpandArm64(const unsigned char* m, const unsigned char* e, const unsigned char* s,
                          char* d, int z) { return Expand(m,e,s,d,z,true); }
int BIONIC_SYSV Skip(const unsigned char** p, const unsigned char* e) { return NameSkip(p,e); }
int BIONIC_SYSV DnSkip(const unsigned char* p, const unsigned char* e) {
  const unsigned char* next = p;
  if (NameSkip(&next,e) < 0) return -1;
  return static_cast<int>(next - p);
}
} // namespace

void RegisterDnsWire(SymbolTable& table) {
  table["ns_name_unpack"] = reinterpret_cast<void*>(&Unpack);
  table["ns_name_ntop"] = reinterpret_cast<void*>(&Ntop);
  table["ns_name_uncompress"] = reinterpret_cast<void*>(&Uncompress);
  table["dn_expand"] = reinterpret_cast<void*>(&ExpandX86);
  table["ns_name_skip"] = reinterpret_cast<void*>(&Skip);
  table["__dn_skipname"] = reinterpret_cast<void*>(&DnSkip);
  table["__gesher_arm64_ns_name_ntop"] = reinterpret_cast<void*>(&NtopArm64);
  table["__gesher_arm64_ns_name_uncompress"] = reinterpret_cast<void*>(&UncompressArm64);
  table["__gesher_arm64_dn_expand"] = reinterpret_cast<void*>(&ExpandArm64);
}
} // namespace gesher::bionic
