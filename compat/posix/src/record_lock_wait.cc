#include "record_lock_wait.h"
#include "internal.h"
#include <bcrypt.h>
#include <sddl.h>
#include <cwchar>

namespace gesher::record_locks {
namespace {
constexpr DWORD kLivenessInterval = 100;
int WinError() { return gesher::ErrnoFromWin32(GetLastError()); }
void Name(const WaitingRequest& request, wchar_t (&name)[128]) {
  // Random request identity, independent of reusable PID/TID/fd/handle values.
  std::swprintf(name, 128, L"Global\\Gesher.RecordWait.v1.%016llx.%016llx",
    static_cast<unsigned long long>(request.token_low), static_cast<unsigned long long>(request.token_high));
}
bool Same(const WaitingRequest& a, const WaitingRequest& b) {
  return a.token_low == b.token_low && a.token_high == b.token_high &&
    a.thread.process.pid == b.thread.process.pid && a.thread.process.created == b.thread.process.created &&
    a.thread.tid == b.thread.tid && a.thread.created == b.thread.created;
}
void Remove(RegistryState* state, const WaitingRequest& request) {
  size_t kept = 0;
  for (size_t i = 0; i < state->wait_count; ++i)
    if (!Same(state->waits[i], request)) state->waits[kept++] = state->waits[i];
  state->wait_count = static_cast<uint32_t>(kept);
}
struct Event {
  HANDLE value = nullptr;
  ~Event() { if (value) CloseHandle(value); }
};
DWORD Remaining(ULONGLONG deadline) {
  ULONGLONG now = GetTickCount64();
  return now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
}
}  // namespace

void NotifyRecordWaiters(const RegistryState& state) {
  for (size_t i = 0; i < state.wait_count; ++i) {
    wchar_t name[128]; Name(state.waits[i], name);
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (event) { SetEvent(event); CloseHandle(event); }
  }
}

int WaitProcessLock(Registry& registry, const PreparedOperation& prepared, HANDLE cancel, DWORD timeout) {
  if (prepared.ofd) return ENOSYS;
  if (prepared.query || prepared.input.type < 0 || prepared.input.type > 1 ||
      !cancel || !timeout || timeout > 60000) return EINVAL;
  const ULONGLONG deadline = GetTickCount64() + timeout;
  WaitingRequest request{};
  int error = CurrentThreadIdentity(&request.thread);
  if (error) return error;
  request.requested = {prepared.file,
    {Ownership::kProcess, request.thread.process.pid, request.thread.process.created},
    prepared.range, static_cast<Kind>(prepared.input.type), static_cast<int32_t>(request.thread.process.pid)};
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&request.token_low),
                                    2 * sizeof(uint64_t), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return EIO;
  if (!(request.token_low || request.token_high)) return EIO;
  PSECURITY_DESCRIPTOR security = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
    L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;AU)", SDDL_REVISION_1, &security, nullptr)) return WinError();
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), security, FALSE};
  wchar_t name[128]; Name(request, name);
  Event event;
  event.value = CreateEventW(&attributes, TRUE, FALSE, name);
  DWORD create_error = GetLastError();
  LocalFree(security);
  if (!event.value) return gesher::ErrnoFromWin32(create_error);
  if (create_error == ERROR_ALREADY_EXISTS) return EEXIST;
  bool registered = false;
  for (;;) {
    RegistryState* state = nullptr;
    error = registry.Begin(&state, Remaining(deadline), cancel);
    if (error) break;
    Pruned pruned{};
    PruneDeadOwners(state, &pruned);  // unknown owners remain conflicts
    Remove(state, request);
    DWORD canceled = WaitForSingleObject(cancel, 0);
    bool acquired = false;
    if (canceled == WAIT_OBJECT_0) error = ECANCELED;
    else if (canceled != WAIT_TIMEOUT) error = canceled == WAIT_FAILED ? WinError() : EINVAL;
    else if (!Remaining(deadline)) error = ETIMEDOUT;
    else if (!FindConflict(state->locks, request.requested)) {
      error = Replace(&state->locks, request.requested, kRecordCapacity);
      acquired = !error;
    } else if (state->wait_count == kWaitCapacity) error = ENOLCK;
    else state->waits[state->wait_count++] = request;
    // Removal and lock acquisition are one transaction. No partial lock is
    // published on capacity/error/cancel. Other owners may proceed immediately
    // after Unlock, including close/dup2/stdio paths in this same process.
    int commit = registry.Commit();
    if (commit) error = commit;
    if (!commit) registered = !error && !acquired;
    // Reset only under the same mutex that protects publication+notification:
    // subsequent commits cannot be lost between reset, unlock and kernel wait.
    if (registered && !ResetEvent(event.value)) error = WinError();
    registry.Unlock();
    if (error || acquired) break;
    HANDLE handles[]{cancel, event.value};
    DWORD wait = WaitForMultipleObjects(2, handles, FALSE,
      (Remaining(deadline) < kLivenessInterval ? Remaining(deadline) : kLivenessInterval));
    if (wait == WAIT_FAILED) { error = WinError(); break; }
    if (wait != WAIT_OBJECT_0 && wait != WAIT_OBJECT_0 + 1 && wait != WAIT_TIMEOUT) { error = EIO; break; }
    // Recheck cancellation/deadline/conflict inside the next transaction.
    // Death and missed hints are recovered by real identity probes, not by
    // declaring a wait interval to be evidence that acquisition occurred.
  }
  if (registered) {
    // Cleanup stays within the original deadline, even for cancellation.
    // If registry access fails, stale request is harmless metadata (no lock);
    // thread death pruning removes it. Never extend the deadline to clean up.
    RegistryState* state = nullptr;
    int cleanup = registry.Begin(&state, Remaining(deadline));
    if (!cleanup) { Remove(state, request); cleanup = registry.Commit(); registry.Unlock(); }
    if (cleanup && error == 0) error = cleanup;
  }
  return error;
}
}  // namespace gesher::record_locks
