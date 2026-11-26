// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "shared_futex.h"

#include "internal.h"
#include "record_lock_liveness.h"

#include <bcrypt.h>
#include <errno.h>
#include <gesher/fork.h>
#include <wchar.h>

#include <new>

namespace gesher {
namespace {

constexpr unsigned kSlotCount = 256;
enum SlotState : LONG { kFree, kWaiting, kWoken };

struct EventToken {
  uint64_t low;
  uint64_t high;
};

void EventName(const EventToken& token, wchar_t (&out)[80]) {
  swprintf(out, 80, L"Local\\Gesher.Futex.Wait.v1.%016llx.%016llx",
           static_cast<unsigned long long>(token.low), static_cast<unsigned long long>(token.high));
}

int LastError() { return ErrnoFromWin32(GetLastError()); }

class QueueLock {
 public:
  explicit QueueLock(HANDLE mutex) : mutex_(mutex) {
    const DWORD result = WaitForSingleObject(mutex, INFINITE);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
      error_ = result == WAIT_FAILED ? LastError() : EIO;
  }
  ~QueueLock() { if (error_ == 0) ReleaseMutex(mutex_); }
  QueueLock(const QueueLock&) = delete;
  QueueLock& operator=(const QueueLock&) = delete;

  int Error() const { return error_; }

 private:
  HANDLE mutex_;
  int error_ = 0;
};

int NewEvent(EventToken* token, HANDLE* event) {
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    const auto status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(token),
                                       sizeof(*token), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) return EIO;
    wchar_t name[80];
    EventName(*token, name);
    HANDLE result = CreateEventW(nullptr, FALSE, FALSE, name);
    if (result == nullptr) return LastError();
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
      *event = result;
      return 0;
    }
    CloseHandle(result);
  }
  return EIO;
}

}  // namespace

// A slot is published by changing its state under the mutex. A process can
// die while holding that mutex. A free slot's other fields are then ignored
// and a published slot still has enough identity to check for owner death.
struct SharedFutexSlot {
  record_locks::ThreadIdentity owner;
  EventToken event;
  uint32_t mask;
  volatile LONG state;
};
static_assert(sizeof(SharedFutexSlot) == 56);

// CloneProcess temporarily makes Win32 handles inheritable. Keep the queue
// views and handles of other threads reachable so the fork child can drop
// its copies, without changing any wait slot owned by a parent thread.
struct SharedFutexResources {
  SharedFutexResources* previous = nullptr;
  SharedFutexResources* next = nullptr;
  HANDLE file = nullptr;
  HANDLE mutex = nullptr;
  HANDLE section = nullptr;
  HANDLE event = nullptr;
  SharedFutexSlot* slots = nullptr;

  void Close(bool fork_child) const {
    if (slots != nullptr) UnmapViewOfFile(slots);
    for (HANDLE handle : {event, section, mutex, file}) {
      if (handle != nullptr && (!fork_child || InheritedAtFork(handle))) CloseHandle(handle);
    }
  }
};

namespace {
std::mutex g_shared_futex_resources_lock;
SharedFutexResources* g_shared_futex_resources = nullptr;

bool FutexResourcesTryLock(void*) { return g_shared_futex_resources_lock.try_lock(); }
void FutexResourcesUnlock(void*) { g_shared_futex_resources_lock.unlock(); }
void FutexResourcesChild(void*) {
  while (g_shared_futex_resources != nullptr) {
    auto* record = g_shared_futex_resources;
    g_shared_futex_resources = record->next;
    record->Close(true);
    delete record;
  }
  new (&g_shared_futex_resources_lock) std::mutex;
}
const ForkLockOps kFutexResourcesOps = {&FutexResourcesTryLock, &FutexResourcesUnlock,
                                       &FutexResourcesChild};
const ForkLockRegistration g_fork_shared_futex_resources(
    kForkOrderLeaf, "shared futex resources", []() -> void* { return &g_shared_futex_resources_lock; },
    &kFutexResourcesOps);
}  // namespace

int SharedFutexMapping::Pin(HANDLE section) {
  std::lock_guard<std::mutex> guard(g_shared_futex_resources_lock);
  auto* record = new (std::nothrow) SharedFutexResources;
  if (record == nullptr) return ENOMEM;
  if (!DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), &record->file,
                       0, FALSE, DUPLICATE_SAME_ACCESS)) {
    const int error = LastError();
    delete record;
    return error;
  }
  record->next = g_shared_futex_resources;
  if (record->next != nullptr) record->next->previous = record;
  g_shared_futex_resources = resources_ = record;
  return 0;
}

SharedFutexMapping::~SharedFutexMapping() {
  if (resources_ == nullptr) return;
  std::lock_guard<std::mutex> guard(g_shared_futex_resources_lock);
  if (resources_->previous != nullptr) resources_->previous->next = resources_->next;
  else g_shared_futex_resources = resources_->next;
  if (resources_->next != nullptr) resources_->next->previous = resources_->previous;
  resources_->Close(false);
  delete resources_;
}

int SharedFutexQueue::Open(bool create) {
  std::lock_guard<std::mutex> guard(g_shared_futex_resources_lock);
  wchar_t section_name[120], mutex_name[120];
  const auto make_name = [this](wchar_t* name, wchar_t kind) {
    swprintf(name, 120, L"Local\\Gesher.Futex.v1.%lc.%016llx.%016llx.%016llx.%016llx", kind,
             static_cast<unsigned long long>(key_.file.volume),
             static_cast<unsigned long long>(key_.file.id_low),
             static_cast<unsigned long long>(key_.file.id_high),
             static_cast<unsigned long long>(key_.offset));
  };
  constexpr DWORD bytes = kSlotCount * sizeof(SharedFutexSlot);
  make_name(section_name, L's');
  if (create) {
    resources_.section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                             bytes, section_name);
  } else {
    resources_.section = OpenFileMappingW(FILE_MAP_WRITE, FALSE, section_name);
    if (resources_.section == nullptr && GetLastError() == ERROR_FILE_NOT_FOUND) return 0;
  }
  if (resources_.section == nullptr) return -LastError();
  make_name(mutex_name, L'm');
  resources_.mutex = CreateMutexW(nullptr, FALSE, mutex_name);
  if (resources_.mutex == nullptr) return -LastError();
  resources_.slots = static_cast<SharedFutexSlot*>(
      MapViewOfFile(resources_.section, FILE_MAP_WRITE, 0, 0, bytes));
  return resources_.slots == nullptr ? -LastError() : 1;
}

int SharedFutexQueue::Enqueue(const volatile int* address, int expected, uint32_t mask,
                             SharedFutexWaiter* waiter) {
  record_locks::ThreadIdentity owner{};
  if (int error = record_locks::CurrentThreadIdentity(&owner)) return error;
  EventToken token;
  {
    std::lock_guard<std::mutex> guard(g_shared_futex_resources_lock);
    if (int error = NewEvent(&token, &resources_.event)) return error;
  }
  waiter->event = resources_.event;

  QueueLock lock(resources_.mutex);
  if (lock.Error()) return lock.Error();
  int actual = 0;
  if (int error = ReadSharedFutexWord(address, key_, &actual)) return error;
  if (actual != expected) return EAGAIN;

  unsigned index = 0;
  for (; index < kSlotCount; ++index) {
    if (resources_.slots[index].state == kFree) break;
  }
  if (index == kSlotCount) {
    // A terminated process or thread can leave slots behind as long as
    // other live waiters retain the shared section. Serialize the temporary
    // process/thread probes with fork's handle snapshot too.
    std::lock_guard<std::mutex> guard(g_shared_futex_resources_lock);
    for (index = 0; index < kSlotCount; ++index) {
      if (record_locks::ProbeThread(resources_.slots[index].owner) == record_locks::Liveness::kDead)
        break;
    }
    if (index == kSlotCount) return ENOMEM;
    InterlockedExchange(&resources_.slots[index].state, kFree);
  }
  SharedFutexSlot& slot = resources_.slots[index];
  slot.owner = owner;
  slot.event = token;
  slot.mask = mask;
  InterlockedExchange(&slot.state, kWaiting);
  waiter->index = index;
  return 0;
}

int SharedFutexQueue::Complete(const SharedFutexWaiter& waiter, bool retire) {
  QueueLock lock(resources_.mutex);
  if (lock.Error()) return -lock.Error();
  SharedFutexSlot& slot = resources_.slots[waiter.index];
  const bool woken = slot.state == kWoken;
  if (retire || woken) InterlockedExchange(&slot.state, kFree);
  return woken;
}

long SharedFutexQueue::Wake(int count, uint32_t mask) {
  QueueLock lock(resources_.mutex);
  if (lock.Error()) return -lock.Error();
  int woken = 0;
  // Match the existing non-PI wake behavior for nonpositive counts.
  const int limit = count > 0 ? count : 1;
  for (unsigned index = 0; index < kSlotCount && woken < limit; ++index) {
    SharedFutexSlot& slot = resources_.slots[index];
    if (slot.state != kWaiting || (slot.mask & mask) == 0) continue;
    std::lock_guard<std::mutex> guard(g_shared_futex_resources_lock);
    if (record_locks::ProbeThread(slot.owner) == record_locks::Liveness::kDead) {
      InterlockedExchange(&slot.state, kFree);
      continue;
    }
    wchar_t name[80];
    EventName(slot.event, name);
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (event == nullptr) {
      const DWORD last_error = GetLastError();
      if (last_error == ERROR_FILE_NOT_FOUND) {
        // The owner closed its last event handle, for example during
        // process termination between the liveness probe and this open.
        InterlockedExchange(&slot.state, kFree);
        continue;
      }
      return woken ? woken : -ErrnoFromWin32(last_error);
    }
    const BOOL signaled = SetEvent(event);
    const int error = signaled ? 0 : LastError();
    CloseHandle(event);
    if (!signaled) return woken ? woken : -error;
    // The waiter cannot inspect the slot until we release the mutex.
    // If we die before committing the wake, the event is merely spurious.
    InterlockedExchange(&slot.state, kWoken);
    ++woken;
  }
  return woken;
}

}  // namespace gesher
