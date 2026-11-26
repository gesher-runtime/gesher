// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// The complete production helper is linked as a separate translation unit.
#include "compat/media/encode/codec_config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
using Bytes = std::vector<uint8_t>;
using Ranges = std::vector<std::pair<size_t, size_t>>;
std::atomic<size_t> checks{}, calls{};

void Check(bool value, const char* label) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL Annex B: %s\n", label);
    std::exit(1);
  }
}

// An independent sequence-search oracle: it neither uses word arithmetic nor
// repeats the production scanner. Interior zero trimming and a missing final
// empty unit deliberately describe the existing helper, not a codec validator.
Ranges Reference(const uint8_t* data, size_t size) {
  Ranges result;
  if (size == 0) return result;
  constexpr std::array<uint8_t, 3> prefix{0, 0, 1};
  const auto* finish = data + size;
  auto* delimiter = std::search(data, finish, prefix.begin(), prefix.end());
  while (delimiter != finish) {
    const auto* start = delimiter + prefix.size();
    const auto* next = std::search(start, finish, prefix.begin(), prefix.end());
    if (next == finish) {
      if (start != finish) result.emplace_back(start - data, finish - start);
      break;
    }
    const auto last = std::find_if(std::make_reverse_iterator(next),
        std::make_reverse_iterator(start), [](uint8_t byte) { return byte != 0; });
    result.emplace_back(start - data, last.base() - start);
    delimiter = next;
  }
  return result;
}

Ranges Actual(const uint8_t* data, size_t size) {
  ++calls;
  Ranges result;
  for (const auto& unit : gesher::media::SplitAnnexB(data, size)) {
    Check(data != nullptr && unit.first >= data && unit.first <= data + size &&
        unit.second <= static_cast<size_t>(data + size - unit.first), "borrowed range stays within input");
    result.emplace_back(unit.first - data, unit.second);
  }
  return result;
}

void Verify(const uint8_t* data, size_t size, const char* label) {
  Bytes before;
  if (size) before.assign(data, data + size);
  Check(Actual(data, size) == Reference(data, size), label);
  Check(size == 0 || std::equal(before.begin(), before.end(), data), "input and delimiter bytes unchanged");
}

void Exact(const Bytes& bytes, const Ranges& expected, const char* label) {
  Check(Actual(bytes.data(), bytes.size()) == expected, label);
  Check(Reference(bytes.data(), bytes.size()) == expected, "independent oracle agrees with explicit contract");
}

void ExplicitCases() {
  Check(Actual(nullptr, 0).empty(), "null empty buffer");
  Exact({0, 0}, {}, "truncated delimiter ignored");
  Exact({0, 0, 1}, {}, "final empty NAL omitted");
  Exact({0, 0, 1, 0, 0, 1}, {{3, 0}}, "consecutive start codes retain interior empty NAL");
  Exact({0, 0, 0, 1, 0x65, 0}, {{4, 2}}, "four-byte start and final trailing zero retained");
  Exact({7, 8, 0, 0, 1, 0x67, 0, 0, 0, 0, 1, 0x68, 0, 0},
      {{5, 1}, {11, 3}}, "leading garbage ignored and only interior zeros trimmed");
  Exact({0, 0, 1, 0, 0, 0, 0, 1, 0x65}, {{3, 0}, {8, 1}}, "all-zero interior NAL trimmed to empty");
  Exact({0, 0, 1, 0x65, 0, 0, 3, 1, 4}, {{3, 6}}, "escaped delimiter stays in payload");
  for (size_t at : {size_t{4}, size_t{7}, size_t{8}, size_t{15}}) {
    Bytes bytes(32, 0x55);
    bytes[at] = bytes[at + 1] = 0;
    bytes[at + 2] = 1;
    Exact(bytes, {{at + 3, bytes.size() - at - 3}}, "prefix inside or crossing a complete word");
  }
  const std::array<uint8_t, 2> first{0, 0}, second{1, 0x65};
  Check(Actual(first.data(), first.size()).empty() && Actual(second.data(), second.size()).empty(),
      "separate input buffers never form a delimiter");
}

void Exhaustive() {
  constexpr uint8_t alphabet[] = {0, 1, 3, 0x80};
  size_t variants = 1;
  for (size_t size = 0; size <= 9; ++size, variants *= 4) {
    Bytes bytes(size);
    for (size_t value = 0; value < variants; ++value) {
      size_t digits = value;
      for (auto& byte : bytes) { byte = alphabet[digits % 4]; digits /= 4; }
      Verify(bytes.data(), size, "exhaustive short sequence and truncation");
    }
  }
  for (unsigned value = 0; value <= 255; ++value) {
    for (size_t alignment = 0; alignment < 16; ++alignment) {
      for (size_t at = 0; at < 48; ++at) {
        Bytes storage(96 + alignment, static_cast<uint8_t>(value));
        auto* data = storage.data() + alignment;
        data[at] = data[at + 1] = 0; data[at + 2] = 1;
        for (size_t end : {at + 1, at + 2, at + 3, at + 4, size_t{96}})
          Verify(data, end, "every byte value, unaligned start and bounded delimiter tail");
      }
    }
  }
}

class Guarded {
 public:
  Guarded() {
#ifdef _WIN32
    SYSTEM_INFO info{}; GetSystemInfo(&info); page_ = info.dwPageSize;
    mapping_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, 3 * page_, MEM_RESERVE, PAGE_NOACCESS));
    Check(mapping_ != nullptr, "reserve guard pages");
    Check(VirtualAlloc(mapping_ + page_, page_, MEM_COMMIT, PAGE_READWRITE) == mapping_ + page_,
        "commit middle input page");
#else
    const long page = sysconf(_SC_PAGESIZE); Check(page > 0, "native page size");
    page_ = static_cast<size_t>(page);
    void* mapping = mmap(nullptr, 3 * page_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(mapping != MAP_FAILED, "reserve guard pages"); mapping_ = static_cast<uint8_t*>(mapping);
    Check(mprotect(mapping_ + page_, page_, PROT_READ | PROT_WRITE) == 0, "commit middle input page");
#endif
  }
  ~Guarded() {
#ifdef _WIN32
    Check(VirtualFree(mapping_, 0, MEM_RELEASE) != 0, "release guarded input allocation");
#else
    Check(munmap(mapping_, 3 * page_) == 0, "release guarded input allocation");
#endif
  }
  uint8_t* Begin() const { return mapping_ + page_; }
  uint8_t* End() const { return mapping_ + 2 * page_; }
 private:
  uint8_t* mapping_{};
  size_t page_{};
};

void PageBoundaries() {
  Guarded guarded;
  for (size_t size = 0; size <= 128; ++size) {
    for (bool at_end : {false, true}) {
      auto* data = at_end ? guarded.End() - size : guarded.Begin();
      for (uint8_t fill : {uint8_t{0}, uint8_t{1}, uint8_t{255}}) {
        if (size) std::memset(data, fill, size);
        Verify(data, size, "exact native protected-page boundary");
        if (size >= 3) {
          data[size - 3] = data[size - 2] = 0; data[size - 1] = 1;
          Verify(data, size, "delimiter ends at protected-page boundary");
        }
      }
    }
  }
}

void LongPatterns(unsigned seed) {
  std::mt19937 random(seed);
  for (size_t size : {size_t{127}, size_t{128}, size_t{129}, size_t{4095}, size_t{4096},
                     size_t{4097}, size_t{65535}, size_t{65536}, size_t{262147}}) {
    for (unsigned kind = 0; kind != 6; ++kind) {
      Bytes bytes(size);
      for (size_t i = 0; i < size; ++i) {
        if (kind < 3) bytes[i] = kind == 0 ? 0 : kind == 1 ? 1 : 255;
        else if (kind == 3) bytes[i] = i % 3 == 2 ? 3 : 0;
        else bytes[i] = static_cast<uint8_t>(random());
      }
      if (kind != 5) {
        for (size_t i = 0; i + 2 < size; i += 127) { bytes[i] = bytes[i + 1] = 0; bytes[i + 2] = 1; }
      }
      Verify(bytes.data(), bytes.size(), "long dense, escaped, one-byte-borrow and random payloads");
    }
  }
}
}  // namespace

int main() {
  ExplicitCases(); Exhaustive(); PageBoundaries(); LongPatterns(56);
  std::array<std::thread, 8> threads;
  for (size_t i = 0; i < threads.size(); ++i)
    threads[i] = std::thread([i] { for (unsigned j = 0; j < 2; ++j) LongPatterns(100 + 2 * i + j); });
  for (auto& thread : threads) thread.join();
  std::printf("Annex B contracts: %zu checks, %zu calls, PASS\n", checks.load(), calls.load());
}
