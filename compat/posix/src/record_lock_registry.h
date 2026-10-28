// Cross-session advisory metadata storage. This is not a lock on file data.
// Descriptor, signal and spawn integration are separate runtime requirements.
#pragma once
#include "record_lock_ranges.h"
#include <windows.h>

namespace gesher::record_locks {

struct ProcessIdentity { uint32_t pid, reserved; uint64_t created; };
struct ThreadIdentity { ProcessIdentity process; uint32_t tid, reserved; uint64_t created; };
struct Reference {
  Owner description;
  File file;
  ProcessIdentity process;
  uint64_t native_handle;
  int32_t descriptor;
  uint32_t flags;
};
struct WaitingRequest {
  ThreadIdentity thread;
  uint64_t token_low, token_high;
  Record requested;
};
constexpr size_t kReferenceCapacity = 2048;
constexpr size_t kWaitCapacity = 256;
struct RegistryState {
  Snapshot locks;
  uint32_t reference_count = 0, wait_count = 0;
  std::array<Reference, kReferenceCapacity> references{};
  std::array<WaitingRequest, kWaitCapacity> waits{};
};

// One instance belongs to one operation/thread. Never share its staged buffer
// concurrently. The persistent secured file, not an ephemeral named section,
// retains metadata when this instance closes its mapping/handles.
class Registry {
 public:
  using Allocate = void* (*)(size_t);
  using Release = void (*)(void*);
  Registry() = default;
  // Explicit internal test dependency; production uses checked Windows heap.
  Registry(Allocate allocate, Release release) : allocate_(allocate), release_(release) {}
  ~Registry();
  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;
  int Open();
  int Begin(RegistryState** state);
  int Commit();
  void Unlock();
  uint64_t Generation() const { return generation_; }

 private:
  int Validate();
  void Reset();
  HANDLE mutex_ = nullptr;
  HANDLE file_ = INVALID_HANDLE_VALUE;
  HANDLE mapping_ = nullptr;
  void* disk_ = nullptr;
  RegistryState* staged_ = nullptr;
  bool held_ = false;
  LONG active_ = -1;
  uint64_t generation_ = 0;
  Allocate allocate_ = nullptr;
  Release release_ = nullptr;
};

}  // namespace gesher::record_locks
