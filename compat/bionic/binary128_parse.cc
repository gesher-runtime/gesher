// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "binary128_parse.h"

#include <bit>
#include <cfloat>
#include <cfenv>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string_view>

extern "C" int gesher_gdtoa_strtorQ(const char*, char**, int, void*);
extern "C" void gesher_gdtoa_hexdig_init(void);
struct Bigint;
extern "C" Bigint* gesher_gdtoa_i2b(int);
extern "C" Bigint* gesher_gdtoa_pow5mult(Bigint*, int);
extern "C" void gesher_gdtoa_Bfree(Bigint*);

// Upstream gdtoa shares a Bigint free list and private arena (lock 0), a
// lazily grown chain of 5^(2^k) Bigints (lock 1) and a lazily filled hex digit
// table. Bionic builds it with MULTIPLE_THREADS, holding each lock only
// briefly, so one long conversion does not stall the others. Two upstream
// details need more than those locks:
//  - pow5mult reads the chain without a lock, while a grower publishes a new
//    node before clearing its `next` field, which Balloc does not reset for a
//    recycled Bigint. A concurrent reader could follow that stale link. Each
//    parse therefore holds a shared gate, and lock 1 (growth only) upgrades
//    to exclusive use of it: no other parse reads the chain meanwhile. The
//    upgrade waits for every parse in flight, and parses that start meanwhile
//    can queue behind it, so the chain is grown once, before the first
//    conversion, far enough for the whole binary128 exponent range (see
//    kPrewarmedChainNodes). Only an input with tens of thousands of fraction
//    digits still grows it later and can then stall other parses.
//  - Balloc and pow5mult return NULL after an allocation failure with their
//    lock still held. WithParseGate releases any such lock when the
//    conversion returns, and lock 0 is re-entrant for this thread meanwhile.
// The hex table is filled once before any conversion uses it.
namespace {

std::shared_mutex g_parse_gate;
std::mutex g_alloc_lock;
enum class Gate { kNone, kShared, kExclusive };
thread_local Gate t_gate = Gate::kNone;
thread_local unsigned t_alloc_depth = 0;

[[noreturn]] void GdtoaLockFailure(const char* what) {
  std::fprintf(stderr, "gesher bionic: gdtoa lock misuse: %s\n", what);
  std::abort();
}

// Changing the gate must not wait while holding lock 0, which another parse
// may need before it can leave the gate. Only a leaked lock 0 is held here.
template <typename Change>
void ChangeGate(Change change) {
  unsigned alloc = t_alloc_depth;
  if (alloc != 0) {
    t_alloc_depth = 0;
    g_alloc_lock.unlock();
  }
  change();
  if (alloc != 0) {
    g_alloc_lock.lock();
    t_alloc_depth = alloc;
  }
}

}  // namespace

extern "C" void gesher_gdtoa_lock(int lock) {
  if (lock == 0) {
    if (t_alloc_depth++ == 0) g_alloc_lock.lock();
    return;
  }
  if (lock != 1 || t_gate != Gate::kShared) GdtoaLockFailure("lock 1 outside a parse");
  ChangeGate([] {
    g_parse_gate.unlock_shared();
    g_parse_gate.lock();
  });
  t_gate = Gate::kExclusive;
}

extern "C" void gesher_gdtoa_unlock(int lock) {
  if (lock == 0) {
    if (t_alloc_depth == 0) GdtoaLockFailure("lock 0 not held");
    if (--t_alloc_depth == 0) g_alloc_lock.unlock();
    return;
  }
  if (lock != 1 || t_gate != Gate::kExclusive) GdtoaLockFailure("lock 1 not held");
  ChangeGate([] {
    g_parse_gate.unlock();
    g_parse_gate.lock_shared();
  });
  t_gate = Gate::kShared;
}

namespace {

// Runs a gdtoa call with this thread inside the parse gate, then releases any
// lock an allocation failure left held.
template <typename Call>
void WithParseGate(Call call) {
  if (t_gate != Gate::kNone) GdtoaLockFailure("nested parse");
  g_parse_gate.lock_shared();
  t_gate = Gate::kShared;
  call();
  if (t_alloc_depth != 0) {
    t_alloc_depth = 0;
    g_alloc_lock.unlock();
  }
  if (t_gate == Gate::kExclusive) g_parse_gate.unlock();
  else g_parse_gate.unlock_shared();
  t_gate = Gate::kNone;
}

// pow5mult(b, k) multiplies by 5^(k & 3) and then by chain node j, which is
// 5^(4 * 2^j), for each set bit j of k >> 2; reaching bit j creates nodes up
// to j. strtodg passes k = |e|, where e is the explicit decimal exponent
// (|e| capped at 19999) less the number of fraction digits. Nodes 0..13 cover
// every k below 2^16: every input with fewer than about 45000 fraction digits,
// which includes the whole binary128 range from the largest finite value to
// the exact 16494-fraction-digit expansion of the least subnormal and its
// halfway points. Building them costs about 1.5 ms once; they stay allocated
// for the process lifetime, as upstream's chain does, in about 20 KiB.
constexpr int kPrewarmedChainNodes = 14;
// pow5mult arguments below this bound use only prewarmed nodes.
constexpr int kPrewarmedPow5Limit = 1 << (kPrewarmedChainNodes + 2);

void PrewarmPowersOfFive() {
  WithParseGate([] {
    Bigint* b = gesher_gdtoa_i2b(1);
    // A single high bit grows the chain with only one multiplication by the
    // last node. Allocation failure leaves a shorter chain to grow later.
    if (b != nullptr) b = gesher_gdtoa_pow5mult(b, kPrewarmedPow5Limit >> 1);
    if (b != nullptr) gesher_gdtoa_Bfree(b);
  });
}

}  // namespace

namespace gesher::bionic {

static_assert(std::endian::native == std::endian::little && CHAR_BIT == 8);
static_assert(sizeof(int) == 4 && sizeof(double) == 8 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024);

int Binary128RoundingMode() {
  switch (std::fegetround()) {
    case FE_TOWARDZERO: return 0;
    case FE_UPWARD: return 2;
    case FE_DOWNWARD: return 3;
    default: return 1;
  }
}

void ParseBinary128(const char* text, char** end, int rounding, void* output) {
  // Only this entry point calls the private, prefixed gdtoa functions; see
  // the locking notes above.
  static std::once_flag init_once;
  std::call_once(init_once, [] {
    gesher_gdtoa_hexdig_init();
    PrewarmPowersOfFive();
  });
  uint32_t words[4];
  // An allocation failure leaves upstream's lock 0 and/or lock 1 held;
  // WithParseGate releases them.
  WithParseGate([&] { gesher_gdtoa_strtorQ(text, end, rounding, words); });
  std::memcpy(output, words, sizeof(words));
}

size_t Binary128WidePrefix(const char* text) {
  // This is the committed-prefix grammar of pinned bionic/stdio/parsefloat.c,
  // used by bionic/wcstod.cpp before the actual numeric conversion.
  std::string_view s(text);
  size_t pos = 0;
  if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
  auto matches = [&](std::string_view word) {
    if (s.size() - pos < word.size()) return false;
    for (size_t i = 0; i < word.size(); ++i) {
      char c = s[pos + i];
      if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
      if (c != word[i]) return false;
    }
    return true;
  };
  if (matches("inf")) return pos + (matches("infinity") ? 8 : 3);
  if (matches("nan")) {
    size_t committed = pos + 3;
    pos = committed;
    if (pos == s.size() || s[pos++] != '(') return committed;
    while (pos < s.size()) {
      char c = s[pos++];
      if (c == ')') return pos;
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || c == '_')) break;
    }
    return committed;
  }
  size_t committed = 0;
  bool hex = s.size() - pos >= 2 && s[pos] == '0' &&
             (s[pos + 1] == 'x' || s[pos + 1] == 'X');
  if (hex) {
    committed = pos + 1;  // An incomplete 0x token still commits its zero.
    pos += 2;
  }
  auto digit = [&](char c) {
    return (c >= '0' && c <= '9') ||
           (hex && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')));
  };
  bool any = false;
  while (pos < s.size() && digit(s[pos])) {
    any = true;
    committed = ++pos;
  }
  if (pos < s.size() && s[pos] == '.') {
    ++pos;
    if (any) committed = pos;
    while (pos < s.size() && digit(s[pos])) {
      any = true;
      committed = ++pos;
    }
  }
  if (any && pos < s.size() &&
      (hex ? (s[pos] == 'p' || s[pos] == 'P') : (s[pos] == 'e' || s[pos] == 'E'))) {
    ++pos;
    if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') committed = ++pos;
  }
  return committed;
}

}  // namespace gesher::bionic
