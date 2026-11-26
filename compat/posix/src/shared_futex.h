// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "record_lock_ranges.h"

#include <windows.h>

namespace gesher {

struct SharedFutexKey {
  record_locks::File file{};
  uint64_t offset = 0;
};

// Keep the file section alive even if the descriptor or entire view is
// retired during a wait. NTFS cannot recycle the file identity while the
// section retains the underlying file.
struct SharedFutexResources;
struct SharedFutexMapping {
  SharedFutexKey key;
  int Pin(HANDLE section);
  SharedFutexMapping() = default;
  ~SharedFutexMapping();
  SharedFutexMapping(const SharedFutexMapping&) = delete;
  SharedFutexMapping& operator=(const SharedFutexMapping&) = delete;

 private:
  friend class SharedFutexQueue;
  SharedFutexResources* resources_ = nullptr;
};

// Positive for a live shared file view, zero for a local address, negative
// errno on failure. Reading during enrollment checks the same key again
// under the mmap registry lock in case the virtual address was replaced.
int GetSharedFutexMapping(const volatile int* address, SharedFutexMapping* mapping);
int ReadSharedFutexWord(const volatile int* address, const SharedFutexKey& key, int* value);

struct SharedFutexWaiter {
  HANDLE event = nullptr;
  unsigned index = 0;
};

class SharedFutexQueue {
 public:
  explicit SharedFutexQueue(const SharedFutexMapping& mapping)
      : key_(mapping.key), resources_(*mapping.resources_) {}
  SharedFutexQueue(const SharedFutexQueue&) = delete;
  SharedFutexQueue& operator=(const SharedFutexQueue&) = delete;

  // Positive with a queue, zero if there are no waiters and create is false,
  // or negative errno.
  int Open(bool create);
  int Enqueue(const volatile int* address, int expected, uint32_t mask,
              SharedFutexWaiter* waiter);
  // 1 for a completed wake, 0 otherwise, negative errno on failure. A wake
  // wins a race with retirement, which removes the slot before returning.
  int Complete(const SharedFutexWaiter& waiter, bool retire);
  long Wake(int count, uint32_t mask);

 private:
  const SharedFutexKey& key_;
  SharedFutexResources& resources_;
};

}  // namespace gesher
