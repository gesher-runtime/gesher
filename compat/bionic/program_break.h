// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace gesher::bionic {

// A real, contiguous program-break arena, independent of the CRT malloc heap.
// Modern Android allocators also need not allocate through brk. Reserving the
// address range consumes no physical pages; growth commits the requested pages
// and shrink decommits complete pages. The bounded address-space reservation is
// reported as ENOMEM when exhausted, without changing the previous break.
class ProgramBreak {
 public:
  static constexpr size_t kCapacity = size_t{1} << 30;

  ProgramBreak() = default;
  ProgramBreak(const ProgramBreak&) = delete;
  ProgramBreak& operator=(const ProgramBreak&) = delete;
  ~ProgramBreak() { if (base_) VirtualFree(base_, 0, MEM_RELEASE); }

  void* Increment(intptr_t increment) {
    std::lock_guard lock(mutex_);
    if (!Initialize()) return Failure();
    size_t requested = size_;
    if (increment >= 0) {
      const auto increase = static_cast<uintptr_t>(increment);
      if (increase > kCapacity - size_) return Failure();
      requested += increase;
    } else {
      // INTPTR_MIN cannot be negated in the signed type.
      const auto decrease = static_cast<uintptr_t>(-(increment + 1)) + 1;
      if (decrease > size_) return Failure();
      requested -= decrease;
    }
    void* previous = static_cast<std::byte*>(base_) + size_;
    if (!Resize(requested)) return Failure();
    return previous;
  }

  int Set(void* address) {
    std::lock_guard lock(mutex_);
    if (!Initialize()) { Failure(); return -1; }
    const auto begin = reinterpret_cast<uintptr_t>(base_);
    const auto end = reinterpret_cast<uintptr_t>(address);
    if (end < begin || end - begin > kCapacity || !Resize(end - begin)) {
      Failure();
      return -1;
    }
    return 0;
  }

 private:
  static void* Failure() {
    errno = ENOMEM;
    return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
  }
  bool Initialize() {
    if (base_) return true;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    page_ = info.dwPageSize;
    if (!page_ || (page_ & (page_ - 1)) != 0 || page_ > kCapacity) return false;
    base_ = VirtualAlloc(nullptr, kCapacity, MEM_RESERVE, PAGE_NOACCESS);
    return base_ != nullptr;
  }
  bool Resize(size_t requested) {
    const size_t rounded = (requested + page_ - 1) & ~(page_ - 1);
    if (rounded > committed_) {
      if (!VirtualAlloc(static_cast<std::byte*>(base_) + committed_, rounded - committed_,
                        MEM_COMMIT, PAGE_READWRITE)) return false;
    } else if (rounded < committed_) {
      if (!VirtualFree(static_cast<std::byte*>(base_) + rounded, committed_ - rounded,
                       MEM_DECOMMIT)) return false;
    }
    committed_ = rounded;
    size_ = requested;
    return true;
  }

  std::mutex mutex_;
  void* base_ = nullptr;
  size_t size_ = 0;
  size_t committed_ = 0;
  size_t page_ = 0;
};

}  // namespace gesher::bionic
