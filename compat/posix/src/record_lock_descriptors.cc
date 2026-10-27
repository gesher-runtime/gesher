#include "record_lock_descriptors.h"
#include "internal.h"
#include <bcrypt.h>
#include <new>

namespace gesher::record_locks {
namespace {
constexpr uint32_t kPrivateAnchor = 1;
SRWLOCK descriptor_lock = SRWLOCK_INIT;
thread_local unsigned descriptor_depth = 0;

struct Anchor {
  bool used = false, close_after_commit = false;
  HANDLE handle = nullptr;
  Owner description{};
  File file{};
};
struct Probe {
  HANDLE process = nullptr, object = nullptr;
  ProcessIdentity identity{};
};
struct LocalState {
  Registry registry;
  ProcessIdentity process{};
  std::array<Anchor, kRecordCapacity> anchors{};
  std::array<Probe, kReferenceCapacity> probes{};
};
// Checked, process-owned allocation: no static destructor races other fd
// users or runs registry/anchor cleanup during CRT/VM destruction.
LocalState* local = nullptr;
int WinError() { return gesher::ErrnoFromWin32(GetLastError()); }
uint64_t Birth(const FILETIME& value) {
  return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
bool SameProcess(const ProcessIdentity& left, const ProcessIdentity& right) {
  return left.pid == right.pid && left.created == right.created;
}
bool Alive(HANDLE process, const ProcessIdentity& expected) {
  FILETIME created{}, exited{}, kernel{}, user{};
  return GetProcessTimes(process, &created, &exited, &kernel, &user) &&
         Birth(created) == expected.created && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}
int Initialize() {
  if (local) return 0;
  void* storage = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(LocalState));
  if (!storage) return ENOMEM;
  auto* candidate = new (storage) LocalState{};
  int error = CurrentProcessIdentity(&candidate->process);
  if (!error) error = candidate->registry.Open();
  if (error) {
    candidate->~LocalState();
    HeapFree(GetProcessHeap(), 0, storage);
    return error;
  }
  local = candidate;
  return 0;
}
void CloseProbes(size_t count) {
  for (size_t index = 0; index < count; ++index) {
    Probe& probe = local->probes[index];
    if (probe.object) CloseHandle(probe.object);
    if (probe.process && probe.process != GetCurrentProcess()) CloseHandle(probe.process);
    probe = {};
  }
}
void CancelPending() {
  for (Anchor& anchor : local->anchors) anchor.close_after_commit = false;
}
int ClosePrivate(Anchor* anchor) {
  if (!SetHandleInformation(anchor->handle, HANDLE_FLAG_PROTECT_FROM_CLOSE, 0)) return WinError();
  if (!CloseHandle(anchor->handle)) return WinError();
  *anchor = {};
  return 0;
}
int CloseCommitted() {
  int first_error = 0;
  for (Anchor& anchor : local->anchors) if (anchor.used && anchor.close_after_commit) {
    int error = ClosePrivate(&anchor);
    if (error && !first_error) first_error = error;
  }
  return first_error;
}
void RemoveAnchorReference(RegistryState* state, const Anchor& anchor) {
  size_t retained = 0;
  for (size_t index = 0; index < state->reference_count; ++index) {
    const Reference& reference = state->references[index];
    if (SameProcess(reference.process, local->process) && reference.description == anchor.description &&
        reference.file == anchor.file && reference.native_handle == reinterpret_cast<uintptr_t>(anchor.handle) &&
        reference.flags == kPrivateAnchor) continue;
    state->references[retained++] = reference;
  }
  state->reference_count = static_cast<uint32_t>(retained);
}

// Count actual protected kernel-object anchors and our exact temporary
// probes. File IDs are insufficient: all probes must compare as the SAME
// FILE_OBJECT. Recheck every owner AFTER the HandleCount snapshot; a process
// dying between an earlier probe and the count must not become a phantom
// subtracted anchor that hides a different live user alias.
bool NoUserAliases(const RegistryState& state, const Owner& owner, const File& file) {
  size_t count = 0;
  bool trustworthy = true;
  for (size_t index = 0; index < state.reference_count && trustworthy; ++index) {
    const Reference& reference = state.references[index];
    if (!(reference.description == owner) || !(reference.file == file)) continue;
    if (reference.flags != kPrivateAnchor || reference.descriptor != -1) { trustworthy = false; break; }
    bool duplicate = false;
    for (size_t earlier = 0; earlier < index; ++earlier) {
      const Reference& previous = state.references[earlier];
      if (previous.description == owner && previous.file == file && SameProcess(previous.process, reference.process) &&
          previous.native_handle == reference.native_handle) { duplicate = true; break; }
    }
    if (duplicate) continue;
    Probe& probe = local->probes[count++];
    probe.identity = reference.process;
    probe.process = SameProcess(reference.process, local->process) ? GetCurrentProcess() :
      OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, reference.process.pid);
    if (!probe.process || !Alive(probe.process, reference.process) ||
        !DuplicateHandle(probe.process, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(reference.native_handle)),
                         GetCurrentProcess(), &probe.object, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
      trustworthy = false; break;
    }
    File actual{};
    bool same = true;
    if (FileIdentity(probe.object, &actual) || !(actual == file) ||
        (count > 1 && (SameDescription(local->probes[0].object, probe.object, &same) || !same))) trustworthy = false;
  }
  uint32_t access = 0, handles = 0;
  if (!count || !trustworthy || HandleAccess(local->probes[0].object, &access, &handles)) trustworthy = false;
  if (trustworthy) {
    for (size_t index = 0; index < count; ++index) {
      if (!Alive(local->probes[index].process, local->probes[index].identity)) trustworthy = false;
    }
  }
  // Any unknown/error/race/negative accounting retains ownership. A larger
  // count includes real aliases or an uncontrolled transfer, never permission
  // to discard the owner based on registered descriptor count alone.
  bool zero = trustworthy && handles == count * 2;
  CloseProbes(count);
  return zero;
}
bool Collect(RegistryState* state, const File& file) {
  bool changed = false;
  for (size_t index = 0; index < state->reference_count; ++index) {
    Reference reference = state->references[index];
    if (!(reference.file == file) || reference.flags != kPrivateAnchor) continue;
    bool earlier = false;
    for (size_t previous = 0; previous < index; ++previous) {
      if (state->references[previous].description == reference.description && state->references[previous].file == file) {
        earlier = true; break;
      }
    }
    if (earlier || !NoUserAliases(*state, reference.description, file)) continue;
    const size_t old_count = state->locks.count;
    RemoveOwner(&state->locks, reference.description, &file);
    changed |= old_count != state->locks.count;
    for (Anchor& anchor : local->anchors) {
      if (anchor.used && anchor.description == reference.description && anchor.file == file) {
        anchor.close_after_commit = true;
      }
    }
  }
  // Remove only anchors this process will actually close after publication.
  // Foreign protected handles remain referenced until their owner cleans up
  // or confirmed process death removes them; no remote close is invented.
  for (const Anchor& anchor : local->anchors) if (anchor.used && anchor.close_after_commit) {
    RemoveAnchorReference(state, anchor);
    changed = true;
  }
  return changed;
}
int FreshOwner(const RegistryState& state, Owner* owner) {
  owner->kind = Ownership::kDescription;
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    uint64_t token[2]{};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(token), sizeof(token),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return EIO;
    if (!(token[0] || token[1])) continue;
    owner->low = token[0]; owner->high = token[1];
    bool exists = false;
    for (size_t index = 0; index < state.reference_count; ++index) {
      if (state.references[index].description == *owner) exists = true;
    }
    if (!exists) return 0;
  }
  return ENOLCK;
}
int Description(RegistryState* state, HANDLE file, const File& identity, bool create,
                Owner* owner, Anchor** allocated) {
  *allocated = nullptr;
  for (Anchor& anchor : local->anchors) if (anchor.used) {
    bool same = false;
    int error = SameDescription(anchor.handle, file, &same);
    if (error) return error;
    if (same) { *owner = anchor.description; return 0; }
  }
  // Atomic spawn registration/adoption is a separate mandatory integration
  // step. Do not infer inherited ownership from a matching disk FileId.
  int error = FreshOwner(*state, owner);
  if (error || !create) return error;
  if (state->reference_count == kReferenceCapacity) return ENOLCK;
  Anchor* slot = nullptr;
  for (Anchor& candidate : local->anchors) if (!candidate.used) { slot = &candidate; break; }
  if (!slot) return ENOLCK;
  HANDLE handle = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) return WinError();
  if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT | HANDLE_FLAG_PROTECT_FROM_CLOSE, HANDLE_FLAG_PROTECT_FROM_CLOSE)) {
    error = WinError(); CloseHandle(handle); return error;
  }
  *slot = {true, false, handle, *owner, identity};
  state->references[state->reference_count++] = {*owner, identity, local->process,
    reinterpret_cast<uintptr_t>(handle), -1, kPrivateAnchor};
  *allocated = slot;
  return 0;
}
}  // namespace

DescriptorGuard::DescriptorGuard() { if (descriptor_depth++ == 0) AcquireSRWLockExclusive(&descriptor_lock); }
DescriptorGuard::~DescriptorGuard() { if (--descriptor_depth == 0) ReleaseSRWLockExclusive(&descriptor_lock); }

int DescriptorFcntl(int descriptor, int command, void* user_flock) {
  const int saved_errno = errno;
  DescriptorGuard guard;
  HANDLE file = gesher::HandleFromFd(descriptor);
  if (file == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  PreparedOperation prepared{};
  int error = PrepareOperation(file, command, user_flock, &prepared);
  if (!error) error = Initialize();
  if (!error) error = CloseCommitted();  // Retry only private anchors whose references were already unpublished.
  RegistryState* state = nullptr;
  if (!error) error = local->registry.Begin(&state);
  if (error) return errno = error, -1;
  Pruned pruned{};
  PruneDeadOwners(state, &pruned);
  bool changed = pruned.references || pruned.process_locks || pruned.description_locks || pruned.waits;
  changed |= Collect(state, prepared.file);
  Owner owner{Ownership::kProcess, local->process.pid, local->process.created};
  Anchor* allocated = nullptr;
  if (prepared.ofd) error = Description(state, file, prepared.file, !prepared.query && prepared.input.type != 2,
                                       &owner, &allocated);
  bool mutated = false;
  if (!error) error = ApplyOperation(state, owner, prepared, &mutated);
  if (!error && (changed || mutated || allocated)) error = local->registry.Commit();
  if (error) {
    CancelPending();
    if (allocated) {
      allocated->close_after_commit = true;
      int cleanup = ClosePrivate(allocated);
      if (cleanup) error = cleanup;
    }
  } else error = CloseCommitted();
  local->registry.Unlock();
  if (error) return errno = error, -1;
  errno = saved_errno;
  return 0;
}

ClosingDescriptor::ClosingDescriptor(int descriptor) : descriptor_(descriptor) {
  if (!local) return;
  error_ = CloseCommitted();
  if (error_) return;
  HANDLE file = gesher::HandleFromFd(descriptor);
  if (file == INVALID_HANDLE_VALUE || GetFileType(file) != FILE_TYPE_DISK) return;
  error_ = FileIdentity(file, &file_);
  if (!error_ && !DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &probe_, 0, FALSE, DUPLICATE_SAME_ACCESS)) error_ = WinError();
  if (!error_) error_ = local->registry.Begin(&staged_);
  if (!error_) {
    begun_ = true;
    if (local->registry.Generation() == UINT64_MAX) error_ = EOVERFLOW;
  }
}
ClosingDescriptor::~ClosingDescriptor() {
  if (probe_) CloseHandle(probe_);
  if (begun_) { CancelPending(); local->registry.Unlock(); }
}
int ClosingDescriptor::Finish(bool closed_or_replaced) {
  if (completed_) return EINVAL;
  completed_ = true;
  if (error_) return error_;
  if (!begun_) return 0;
  bool same = false;
  int error = 0;
  if (!closed_or_replaced) {
    HANDLE current = gesher::HandleFromFd(descriptor_);
    error = current == INVALID_HANDLE_VALUE ? 0 : SameDescription(probe_, current, &same);
  }
  if (!CloseHandle(probe_)) return WinError();
  probe_ = nullptr;  // Not a user alias; exclude it from the subsequent count.
  if (error || (same && !closed_or_replaced)) return error;
  Pruned pruned{};
  PruneDeadOwners(staged_, &pruned);
  Owner process{Ownership::kProcess, local->process.pid, local->process.created};
  RemoveOwner(&staged_->locks, process, &file_);
  Collect(staged_, file_);
  error = local->registry.Commit();
  if (error) CancelPending();
  else error = CloseCommitted();
  local->registry.Unlock(); begun_ = false;
  return error;
}

}  // namespace gesher::record_locks
