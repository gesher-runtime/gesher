// CI-only controls of the real registry and finite wait implementation.
#pragma once
#include "../../compat/posix/src/record_lock_test_hooks.h"
#include <atomic>
#include <cwchar>

namespace cleanup_controls {
inline void Require(bool ok, const char* label) {
  if (!ok) { std::fprintf(stderr, "CLEANUP_FAILURE %s error=%lu\n", label,
                          static_cast<unsigned long>(GetLastError())); std::_Exit(110); }
}
inline void Name(const locks::WaitingRequest& request, wchar_t (&name)[128]) {
  std::swprintf(name, 128, L"Global\\Gesher.RecordWait.v1.%016llx.%016llx",
    static_cast<unsigned long long>(request.token_low), static_cast<unsigned long long>(request.token_high));
}
inline void Close(HANDLE handle) {
  Require(CloseHandle(handle), "close owned event");
  DWORD flags = 0;
  Require(!GetHandleInformation(handle, &flags) && GetLastError() == ERROR_INVALID_HANDLE,
          "closed event handle immediately invalid");
}
inline int Run(const locks::PreparedOperation& prepared, HANDLE cancel, const char* mode) {
  const bool timeout = std::strstr(mode, "timeout") != nullptr;
  const bool begin = std::strstr(mode, "begin") != nullptr;
  locks::WaitTestHooks failure;
  failure.fail_begin = begin; failure.fail_commit = !begin;
  HANDLE returned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Require(returned && release, "create thread lifetime controls");
  std::atomic<int> result{-1};
  std::atomic<DWORD> owner_tid{0};
  std::thread owner([&] {
    owner_tid.store(GetCurrentThreadId());
    locks::wait_test_hooks = &failure;
    locks::Registry registry;
    int error = registry.Open();
    if (!error) error = locks::WaitProcessLock(registry, prepared, cancel, timeout ? 1000 : 5000);
    result.store(error);
    Require(SetEvent(returned), "publish actual return");
    // Stay genuinely alive after the request event destructor has run.
    Require(WaitForSingleObject(release, 15000) == WAIT_OBJECT_0, "owner release within bound");
    locks::wait_test_hooks = nullptr;
  });
  locks::Registry observer; locks::RegistryState* state = nullptr;
  Require(observer.Open() == 0, "open observer");
  locks::WaitingRequest original{};
  HANDLE retained = nullptr;
  ULONGLONG deadline = GetTickCount64() + 3000;
  while (!retained && GetTickCount64() < deadline) {
    Require(observer.Begin(&state, 1000) == 0, "observe registration");
    for (size_t i = 0; i < state->wait_count; ++i) {
      if (state->waits[i].thread.process.pid != GetCurrentProcessId() ||
          !(state->waits[i].requested.file == prepared.file)) continue;
      Require(state->wait_count == 1, "isolated original request");
      original = state->waits[i];
      wchar_t name[128]; Name(original, name);
      retained = OpenEventW(SYNCHRONIZE, FALSE, name);
      Require(retained != nullptr, "retain actual published request event");
    }
    observer.Unlock();
    if (!retained) Sleep(1);
  }
  Require(retained != nullptr, "actual request observed before cancellation/timeout");
  HANDLE owner_handle = OpenThread(SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, FALSE, original.thread.tid);
  auto live = [&] {
    Require(WaitForSingleObject(owner_handle, 0) == WAIT_TIMEOUT, "original thread remains live");
  };
  FILETIME created{}, exit{}, kernel{}, user{};
  Require(owner_handle && owner_tid.load() == original.thread.tid &&
          GetThreadTimes(owner_handle, &created, &exit, &kernel, &user) &&
          ((static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime) == original.thread.created,
          "actual owner creation identity matches published request");
  if (!timeout) Require(SetEvent(cancel), "cancel original registered request");
  Require(WaitForSingleObject(returned, 6000) == WAIT_OBJECT_0, "actual finite return");
  live();
  Require(result.load() == (timeout ? ETIMEDOUT : ECANCELED), "original outcome preserved");
  Require(failure.begin_failures == (begin ? 1u : 0u) &&
          failure.commit_failures == (begin ? 0u : 1u), "exact final cleanup injection consumed");
  Require(observer.Begin(&state, 1000) == 0, "read authoritative bank after failed cleanup");
  Require(state->wait_count == 1 && state->waits[0].token_low == original.token_low &&
          state->waits[0].token_high == original.token_high, "failed cleanup retains authoritative request");
  // Fill every other slot with valid live requests and real named events.
  // Only the abandoned original can free capacity in the following controls.
  HANDLE fillers[locks::kWaitCapacity - 1]{};
  for (size_t i = 0; i < locks::kWaitCapacity - 1; ++i) {
    auto request = original;
    request.token_high += i + 1;
    wchar_t name[128]; Name(request, name);
    fillers[i] = CreateEventW(nullptr, TRUE, FALSE, name);
    Require(fillers[i] && GetLastError() != ERROR_ALREADY_EXISTS, "unique capacity event");
    state->waits[state->wait_count++] = request;
  }
  Require(observer.Commit() == 0, "publish full capacity control"); observer.Unlock();
  HANDLE later_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Require(later_cancel != nullptr, "later uncanceled event");
  auto attempt = [&](DWORD injected, int expected, size_t count) {
    live();
    locks::WaitTestHooks probe;
    probe.token_low = original.token_low; probe.token_high = original.token_high;
    probe.event_error = injected;
    locks::wait_test_hooks = &probe;
    locks::Registry later;
    Require(later.Open() == 0, "open later waiter");
    Require(locks::WaitProcessLock(later, prepared, later_cancel, 100) == expected, "later capacity/result");
    locks::wait_test_hooks = nullptr;
    Require(!injected || probe.event_failures == 1, "targeted unknown event probe consumed");
    Require(observer.Begin(&state, 1000) == 0, "observe later authoritative state");
    Require(state->wait_count == count, "exact capacity after later wait");
    Require(locks::FindConflict(state->locks, original.requested) != nullptr, "real holder remains a conflict");
    bool found = false;
    for (size_t i = 0; i < state->wait_count; ++i)
      if (state->waits[i].token_low == original.token_low && state->waits[i].token_high == original.token_high) found = true;
    Require(found == (count == locks::kWaitCapacity), "only abandoned original slot reclaimed");
    observer.Unlock(); live();
  };
  attempt(0, ENOLCK, locks::kWaitCapacity);  // Live observer delays expiration.
  Close(retained);
  wchar_t name[128]; Name(original, name);
  HANDLE absent = OpenEventW(SYNCHRONIZE, FALSE, name);
  Require(!absent && GetLastError() == ERROR_FILE_NOT_FOUND, "original event absent after observer closes");
  attempt(ERROR_ACCESS_DENIED, ENOLCK, locks::kWaitCapacity);
  attempt(ERROR_INVALID_DATA, ENOLCK, locks::kWaitCapacity);
  attempt(0, ETIMEDOUT, locks::kWaitCapacity - 1);  // Actual missing-event reclaim, registration, timeout, cleanup.
  Require(observer.Begin(&state, 1000) == 0, "begin fixture metadata cleanup");
  Require(state->wait_count == locks::kWaitCapacity - 1, "only fixture filler requests remain");
  state->wait_count = 0;
  Require(observer.Commit() == 0, "remove owned fixture requests"); observer.Unlock();
  for (HANDLE filler : fillers) Close(filler);
  Close(later_cancel);
  live(); Require(SetEvent(release), "release original live thread"); owner.join();
  Require(WaitForSingleObject(owner_handle, 0) == WAIT_OBJECT_0, "actual owner thread joined");
  Close(owner_handle);
  Close(returned); Close(release);
  std::printf("CLEANUP_CONTROL mode=%s result=%d begin_failures=%u commit_failures=%u live_identity=1 retained_capacity=256 denied_capacity=256 unknown_capacity=256 reclaimed_capacity=255 later_timeout=1 handles_closed=260\n",
    mode, result.load(), failure.begin_failures, failure.commit_failures);
  std::fflush(stdout);
  return 0;
}
}  // namespace cleanup_controls
