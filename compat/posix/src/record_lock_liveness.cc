#include "record_lock_liveness.h"
#include "internal.h"
#include <cstring>

namespace gesher::record_locks {
namespace {

uint64_t Time(const FILETIME& value) {
  return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
int WinError() { return gesher::ErrnoFromWin32(GetLastError()); }

struct BasicObject {
  ULONG attributes, granted_access, handle_count, pointer_count, reserved[10];
};
static_assert(sizeof(BasicObject) == 56);

}  // namespace

int CurrentProcessIdentity(ProcessIdentity* identity) {
  if (!identity) return EINVAL;
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return WinError();
  *identity = {GetCurrentProcessId(), 0, Time(created)};
  return identity->created == 0 ? EIO : 0;
}

int CurrentThreadIdentity(ThreadIdentity* identity) {
  if (!identity) return EINVAL;
  int error = CurrentProcessIdentity(&identity->process);
  if (error) return error;
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return WinError();
  identity->tid = GetCurrentThreadId();
  identity->reserved = 0;
  identity->created = Time(created);
  return identity->created == 0 ? EIO : 0;
}

Liveness ProbeProcess(const ProcessIdentity& identity) {
  if (identity.pid == 0 || identity.created == 0) return Liveness::kUnknown;
  HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, identity.pid);
  if (!process) {
    // With these supported access flags, ERROR_INVALID_PARAMETER means no
    // such nonzero PID. Access denied/other query failure retains the owner.
    return GetLastError() == ERROR_INVALID_PARAMETER ? Liveness::kDead : Liveness::kUnknown;
  }
  FILETIME created{}, exited{}, kernel{}, user{};
  Liveness result = Liveness::kUnknown;
  if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
    if (Time(created) != identity.created) result = Liveness::kDead;  // Confirmed PID reuse.
    else {
      DWORD status = WaitForSingleObject(process, 0);
      if (status == WAIT_OBJECT_0) result = Liveness::kDead;
      else if (status == WAIT_TIMEOUT) result = Liveness::kAlive;
    }
  }
  CloseHandle(process);
  return result;
}

Liveness ProbeThread(const ThreadIdentity& identity) {
  Liveness process = ProbeProcess(identity.process);
  if (process != Liveness::kAlive) return process;
  if (identity.tid == 0 || identity.created == 0) return Liveness::kUnknown;
  HANDLE thread = OpenThread(SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, FALSE, identity.tid);
  if (!thread) return GetLastError() == ERROR_INVALID_PARAMETER ? Liveness::kDead : Liveness::kUnknown;
  FILETIME created{}, exited{}, kernel{}, user{};
  Liveness result = Liveness::kUnknown;
  DWORD pid = GetProcessIdOfThread(thread);
  if (pid != 0 && pid != identity.process.pid) result = Liveness::kDead;
  else if (pid != 0 && GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
    if (Time(created) != identity.created) result = Liveness::kDead;  // Confirmed TID reuse.
    else {
      DWORD status = WaitForSingleObject(thread, 0);
      if (status == WAIT_OBJECT_0) result = Liveness::kDead;
      else if (status == WAIT_TIMEOUT) result = Liveness::kAlive;
    }
  }
  CloseHandle(thread);
  return result;
}

int FileIdentity(HANDLE file, File* identity) {
  if (!identity) return EINVAL;
  if (file == nullptr || file == INVALID_HANDLE_VALUE) return EBADF;
  if (GetFileType(file) != FILE_TYPE_DISK) return EINVAL;
  FILE_ID_INFO info{};
  if (!GetFileInformationByHandleEx(file, FileIdInfo, &info, sizeof(info))) return WinError();
  identity->volume = info.VolumeSerialNumber;
  std::memcpy(&identity->id_low, info.FileId.Identifier, sizeof(uint64_t));
  std::memcpy(&identity->id_high, info.FileId.Identifier + sizeof(uint64_t), sizeof(uint64_t));
  return 0;
}

int SameDescription(HANDLE first, HANDLE second, bool* same) {
  if (!same) return EINVAL;
  if (!first || !second || first == INVALID_HANDLE_VALUE || second == INVALID_HANDLE_VALUE) return EBADF;
  using Compare = BOOL(WINAPI*)(HANDLE, HANDLE);
  HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
  if (!kernelbase) return EOPNOTSUPP;
  auto compare = reinterpret_cast<Compare>(reinterpret_cast<void*>(GetProcAddress(kernelbase, "CompareObjectHandles")));
  if (!compare) return EOPNOTSUPP;
  // The public Windows10+ API compares underlying kernel objects without
  // requiring file read/write rights. A failure other than NOT_SAME_OBJECT
  // is not evidence that a reused/invalid handle denotes a different owner.
  SetLastError(ERROR_SUCCESS);
  if (compare(first, second)) { *same = true; return 0; }
  DWORD error = GetLastError();
  if (error == ERROR_NOT_SAME_OBJECT) { *same = false; return 0; }
  return error == ERROR_SUCCESS ? EIO : gesher::ErrnoFromWin32(error);
}

int HandleAccess(HANDLE handle, uint32_t* access, uint32_t* handle_count) {
  if (!access) return EINVAL;
  using Query = LONG(WINAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
  using Error = ULONG(WINAPI*)(LONG);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) return EOPNOTSUPP;
  auto query = reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryObject")));
  auto status_error = reinterpret_cast<Error>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlNtStatusToDosError")));
  if (!query || !status_error) return EOPNOTSUPP;
  BasicObject info{};
  LONG status = query(handle, 0 /* ObjectBasicInformation */, &info, sizeof(info), nullptr);
  if (status < 0) return gesher::ErrnoFromWin32(status_error(status));
  *access = info.granted_access;
  if (handle_count) *handle_count = info.handle_count;
  return 0;
}

void PruneDeadOwners(RegistryState* state, Pruned* result, const Probes& probes) {
  *result = {};
  size_t retained = 0;
  for (size_t index = 0; index < state->reference_count; ++index) {
    const Reference& reference = state->references[index];
    Liveness status = probes.process(reference.process);
    if (status == Liveness::kDead) { ++result->references; continue; }
    if (status == Liveness::kUnknown) ++result->unknown;
    state->references[retained++] = reference;
  }
  state->reference_count = static_cast<uint32_t>(retained);
  retained = 0;
  for (size_t index = 0; index < state->locks.count; ++index) {
    const Record& record = state->locks.records[index];
    if (record.owner.kind == Ownership::kProcess) {
      ProcessIdentity owner{static_cast<uint32_t>(record.pid), 0, record.owner.high};
      Liveness status = probes.process(owner);
      if (status == Liveness::kDead) { ++result->process_locks; continue; }
      if (status == Liveness::kUnknown) ++result->unknown;
    } else {
      bool referenced = false;
      for (size_t ref = 0; ref < state->reference_count; ++ref) {
        if (state->references[ref].description == record.owner && state->references[ref].file == record.file) {
          referenced = true;
          break;
        }
      }
      if (!referenced) { ++result->description_locks; continue; }
    }
    state->locks.records[retained++] = record;
  }
  state->locks.count = retained;
  retained = 0;
  for (size_t index = 0; index < state->wait_count; ++index) {
    const WaitingRequest& waiting = state->waits[index];
    Liveness status = probes.thread(waiting.thread);
    if (status == Liveness::kDead) { ++result->waits; continue; }
    if (status == Liveness::kUnknown) ++result->unknown;
    state->waits[retained++] = waiting;
  }
  state->wait_count = static_cast<uint32_t>(retained);
}

}  // namespace gesher::record_locks
