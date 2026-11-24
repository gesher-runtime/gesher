// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Real Windows VM checks. No modeled allocation or fabricated process data.
#include "../../compat/bionic/program_break.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <thread>

namespace {
int checks = 0;
int failures = 0;
void Check(bool condition, const char* name) {
  ++checks;
  if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}
}

int main() {
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  const size_t page = system.dwPageSize;
  void* address = nullptr;
  {
    gesher::bionic::ProgramBreak arena;
    auto* first = static_cast<std::byte*>(arena.Increment(0));
    if (first == reinterpret_cast<void*>(static_cast<intptr_t>(-1))) return 2;
    address = first;
    MEMORY_BASIC_INFORMATION info{};
    Check(VirtualQuery(first, &info, sizeof(info)) == sizeof(info) && info.State == MEM_RESERVE,
          "query reserves address space without claiming committed memory");
    Check(arena.Increment(static_cast<intptr_t>(page + 1)) == first,
          "growth returns the previous exact byte break");
    first[0] = std::byte{0x5a};
    first[page] = std::byte{0x6b};
    Check(arena.Increment(0) == first + page + 1 && first[0] == std::byte{0x5a} &&
              first[page] == std::byte{0x6b}, "grown pages are actually writable");
    Check(arena.Increment(-1) == first + page + 1 && arena.Increment(0) == first + page,
          "shrink preserves the exact prior break");
    Check(VirtualQuery(first + page, &info, sizeof(info)) == sizeof(info) && info.State == MEM_RESERVE,
          "shrink decommits a complete unused page");
    Check(first[0] == std::byte{0x5a}, "shrink preserves retained bytes");
    Check(arena.Increment(1) == first + page && first[page] == std::byte{},
          "recommitted page has Windows zero initialization");
    errno = 0;
    Check(arena.Increment(std::numeric_limits<intptr_t>::min()) ==
              reinterpret_cast<void*>(static_cast<intptr_t>(-1)) && errno == ENOMEM,
          "negative overflow fails honestly");
    errno = 0;
    Check(arena.Increment(std::numeric_limits<intptr_t>::max()) ==
              reinterpret_cast<void*>(static_cast<intptr_t>(-1)) && errno == ENOMEM &&
              arena.Increment(0) == first + page + 1, "failed growth preserves the old break");
    Check(arena.Set(first) == 0 && arena.Increment(0) == first, "brk can release all committed pages");
    errno = 0;
    Check(arena.Set(nullptr) == -1 && errno == ENOMEM && arena.Increment(0) == first,
          "an address outside the owned arena is rejected");
    constexpr size_t count = 1024;
    std::array<std::array<uintptr_t, count>, 2> ranges{};
    auto allocate = [&](size_t thread) {
      for (size_t i = 0; i < count; ++i) ranges[thread][i] =
          reinterpret_cast<uintptr_t>(arena.Increment(1));
    };
    std::thread left(allocate, 0), right(allocate, 1);
    left.join(); right.join();
    std::array<uintptr_t, count * 2> ordered{};
    std::copy(ranges[0].begin(), ranges[0].end(), ordered.begin());
    std::copy(ranges[1].begin(), ranges[1].end(), ordered.begin() + count);
    std::sort(ordered.begin(), ordered.end());
    bool contiguous = true;
    for (size_t i = 0; i < ordered.size(); ++i) {
      if (ordered[i] != reinterpret_cast<uintptr_t>(first) + i) contiguous = false;
    }
    Check(contiguous && arena.Increment(0) == first + ordered.size(),
          "concurrent increments allocate disjoint contiguous bytes");
  }
  MEMORY_BASIC_INFORMATION info{};
  Check(VirtualQuery(address, &info, sizeof(info)) == sizeof(info) && info.State == MEM_FREE,
        "destroying the arena releases the real reservation");
  std::printf("Windows program break contracts: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
