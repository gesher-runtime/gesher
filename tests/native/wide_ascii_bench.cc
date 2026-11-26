// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Caller translation unit: actual conversion bodies are separately compiled,
// without LTO. Timed calls cannot specialize on these generated inputs.
#include "wide_ascii_api.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <time.h>
#include <vector>

#ifdef WIDE_RESOURCE_COUNTS
namespace allocation {
bool active = false;
uint64_t calls = 0, frees = 0, requested = 0, live = 0, peak = 0;
struct alignas(std::max_align_t) Header { size_t size; bool counted; };
}
void* operator new(size_t size) {
  using namespace allocation;
  auto* header = static_cast<Header*>(std::malloc(sizeof(Header) + size));
  if (!header) throw std::bad_alloc();
  header->size = size;
  header->counted = active;
  if (active) { ++calls; requested += size; live += size; peak = std::max(peak, live); }
  return header + 1;
}
void operator delete(void* pointer) noexcept {
  if (!pointer) return;
  using namespace allocation;
  auto* header = static_cast<Header*>(pointer) - 1;
  if (header->counted) { ++frees; live -= header->size; }
  std::free(header);
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, size_t) noexcept { ::operator delete(p); }
#endif

namespace {
namespace g = gesher::bionic;
enum class Mode { bounded, measure, restartable, legacy, chunks, lifecycle };
struct Case {
  std::string name;
  Mode mode = Mode::bounded;
  std::vector<uint32_t> input;
  std::vector<char> output, expected;
  size_t bound = 0, capacity = 0, result = 0, position = SIZE_MAX, loops = 0;
  int error = 17;
  uint64_t full_hash = 0;
};
[[noreturn]] void Die(const char* message) {
  std::fprintf(stderr, "WIDE_BENCH_FAIL %s\n", message);
  std::exit(1);
}
void Require(bool value, const char* message) { if (!value) Die(message); }
uint64_t Hash(const char* bytes, size_t size) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < size; ++i) hash = (hash ^ static_cast<unsigned char>(bytes[i])) * UINT64_C(1099511628211);
  return hash;
}
uint64_t StateHash(const g::BionicMbstate& state) {
  return Hash(reinterpret_cast<const char*>(&state), sizeof(state));
}
uint64_t Fingerprint(size_t result, size_t position, int error,
                     const g::BionicMbstate& state, uint64_t full_hash) {
  return result ^ (position * UINT64_C(0x9e3779b97f4a7c15)) ^
      (static_cast<uint64_t>(error) << 32) ^ StateHash(state) ^ full_hash;
}
g::BionicMbstate State() { return {{0, 0, 0, 0}, {0xa5, 0xa5, 0xa5, 0xa5}}; }

// Independent scalar byte construction, used before the first production call.
// The exhaustive pinned-source/guard corpus is a separate qualification gate.
std::vector<char> Encoding(uint32_t c) {
  if (c < 0x80) return {static_cast<char>(c)};
  unsigned length = c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
  std::vector<char> bytes(length);
  for (unsigned i = length - 1; i > 0; --i) {
    bytes[i] = static_cast<char>(0x80 + c % 64);
    c /= 64;
  }
  bytes[0] = static_cast<char>((length == 2 ? 0xc0 : length == 3 ? 0xe0 : 0xf0) + c);
  return bytes;
}
Case Input(const char* name) {
  Case c;
  c.name = name;
  size_t size = 4096;
  if (c.name == "empty") size = 0;
  else if (c.name == "ascii1") size = 1;
  else if (c.name == "ascii7") size = 7;
  else if (c.name == "ascii32") size = 32;
  else if (c.name == "ascii256") size = 256;
  else if (c.name == "ascii65536") size = 65536;
  else Require(c.name == "ascii4096" || c.name == "measure4096" ||
      c.name == "restart4096" || c.name == "legacy4096" || c.name == "chunks4096" ||
      c.name == "lifecycle4096" || c.name == "mixed4096" || c.name == "nonascii4096" ||
      c.name == "invalid4096" || c.name == "output0" || c.name == "output8" ||
      c.name == "source4096", "known predeclared case");
  c.input.resize(size + 1);
  for (size_t i = 0; i < size; ++i) c.input[i] = 32 + (i * 17) % 95;
  if (c.name == "mixed4096") for (size_t i = 7; i < size; i += 8) c.input[i] = 0x1f642;
  if (c.name == "nonascii4096") {
    constexpr uint32_t values[] = {0x80, 0x7ff, 0x800, 0xd800, 0x1f642, 0x1fffff};
    for (size_t i = 0; i < size; ++i) c.input[i] = values[i % 6];
  }
  if (c.name == "invalid4096") c.input[size - 1] = 0x200000;
  if (c.name == "measure4096") c.mode = Mode::measure;
  if (c.name == "restart4096") c.mode = Mode::restartable;
  if (c.name == "legacy4096") c.mode = Mode::legacy;
  if (c.name == "chunks4096") c.mode = Mode::chunks;
  if (c.name == "lifecycle4096") c.mode = Mode::lifecycle;
  c.bound = c.name == "source4096" ? size : c.input.size();
  c.capacity = c.name == "output0" ? 0 : c.name == "output8" ? 8 : size * 4 + 1;
  c.output.assign(size * 4 + 5, '\x55');
  c.expected = c.output;
  std::vector<char> complete;
  size_t position = 0;
  for (; position < c.bound && (c.mode == Mode::measure || complete.size() < c.capacity); ++position) {
    uint32_t value = c.input[position];
    if (value == 0) { c.position = SIZE_MAX; break; }
    if (value > 0x1fffff) { c.result = SIZE_MAX; c.error = 84; c.position = position; break; }
    auto bytes = Encoding(value);
    if (c.mode != Mode::measure && bytes.size() > c.capacity - complete.size()) break;
    complete.insert(complete.end(), bytes.begin(), bytes.end());
  }
  if (c.result != SIZE_MAX) {
    c.result = complete.size();
    if (position == c.bound || (position < c.input.size() && c.input[position] != 0)) c.position = position;
  }
  if (c.mode == Mode::measure || c.mode == Mode::legacy) c.position = 0;
  if (c.mode != Mode::measure && c.mode != Mode::lifecycle) {
    std::copy(complete.begin(), complete.end(), c.expected.begin() + 2);
    if (c.result != SIZE_MAX && c.result < c.capacity && position < c.bound && c.input[position] == 0)
      c.expected[complete.size() + 2] = 0;
  }
  if (c.mode == Mode::lifecycle) {
    complete.push_back(0);
    c.full_hash = Hash(complete.data(), complete.size());
  }
  size_t work = c.name == "output0" || c.name == "output8" ? 1 : std::max<size_t>(1, size);
  c.loops = std::clamp<size_t>((1u << 21) / work, 16, 262144);
  return c;
}

uint64_t Once(Case& c) {
  auto state = State();
  const uint32_t* cursor = c.input.data();
  size_t result = 0;
  uint64_t full_hash = 0;
  errno = 17;
  switch (c.mode) {
    case Mode::bounded:
      result = g::Wcsnrtombs(c.output.data() + 2, &cursor, c.bound, c.capacity, &state); break;
    case Mode::measure:
      result = g::Wcsnrtombs(nullptr, &cursor, c.bound, 0, &state); break;
    case Mode::restartable:
      result = g::Wcsrtombs(c.output.data() + 2, &cursor, c.capacity, &state); break;
    case Mode::legacy:
      result = g::Wcstombs(c.output.data() + 2, cursor, c.capacity); break;
    case Mode::chunks:
      while (cursor != nullptr) {
        size_t bytes = g::Wcsnrtombs(c.output.data() + 2 + result, &cursor,
            c.input.size() - (cursor - c.input.data()), 7, &state);
        Require(bytes != SIZE_MAX, "bounded chunk succeeds");
        result += bytes;
      }
      break;
    case Mode::lifecycle: {
      size_t bytes = g::Wcsrtombs(nullptr, &cursor, 0, &state);
      Require(bytes != SIZE_MAX, "lifecycle size query");
      std::unique_ptr<char[]> output(new char[bytes + 1]);
      result = g::Wcsrtombs(output.get(), &cursor, bytes + 1, &state);
      Require(result == bytes && cursor == nullptr, "complete lifecycle conversion");
      full_hash = Hash(output.get(), bytes + 1);
      break;
    }
  }
  int error = errno;
  size_t position = cursor == nullptr ? SIZE_MAX : static_cast<size_t>(cursor - c.input.data());
  return Fingerprint(result, position, error, state, full_hash);
}
uint64_t Clock(clockid_t id) {
  timespec t{};
  Require(clock_gettime(id, &t) == 0, "clock available");
  return static_cast<uint64_t>(t.tv_sec) * UINT64_C(1000000000) + t.tv_nsec;
}
void Verify(const Case& c, uint64_t checksum, size_t loops) {
  Require(checksum == loops * Fingerprint(c.result, c.position, c.error, State(), c.full_hash),
          "all iteration results/state/errno/source/consumer checksums");
  Require(c.output == c.expected, "complete output, boundaries and no overwrite");
}
}  // namespace

int main(int argc, char** argv) {
  Require(argc == 2, "one case argument");
  Case c = Input(argv[1]);
  uint64_t original = Hash(reinterpret_cast<const char*>(c.input.data()), c.input.size() * sizeof(uint32_t));
#ifdef WIDE_RESOURCE_COUNTS
  allocation::active = true;
  uint64_t checksum = Once(c);
  allocation::active = false;
  Verify(c, checksum, 1);
  Require(allocation::live == 0 && allocation::calls == allocation::frees, "complete allocation retirement");
  std::printf("{\"case\":\"%s\",\"allocations\":%llu,\"frees\":%llu,\"requested_bytes\":%llu,\"peak_live_requested_bytes\":%llu,\"retained_bytes\":%llu,\"checksum\":%llu}\n",
      c.name.c_str(), (unsigned long long)allocation::calls, (unsigned long long)allocation::frees,
      (unsigned long long)allocation::requested, (unsigned long long)allocation::peak,
      (unsigned long long)allocation::live, (unsigned long long)checksum);
#else
  for (int interval = 0; interval < 4; ++interval) {
    size_t loops = interval == 0 ? 1 : c.loops;
    uint64_t wall = Clock(CLOCK_MONOTONIC), cpu = Clock(CLOCK_PROCESS_CPUTIME_ID), checksum = 0;
    for (size_t i = 0; i < loops; ++i) checksum += Once(c);
    cpu = Clock(CLOCK_PROCESS_CPUTIME_ID) - cpu;
    wall = Clock(CLOCK_MONOTONIC) - wall;
    Verify(c, checksum, loops);
    std::printf("{\"case\":\"%s\",\"interval\":%d,\"loops\":%zu,\"source_words\":%zu,\"cpu_ns\":%llu,\"wall_ns\":%llu,\"checksum\":%llu}\n",
        c.name.c_str(), interval, loops, c.input.size() - 1,
        (unsigned long long)cpu, (unsigned long long)wall, (unsigned long long)checksum);
  }
#endif
  Require(original == Hash(reinterpret_cast<const char*>(c.input.data()), c.input.size() * sizeof(uint32_t)), "input unchanged");
}
