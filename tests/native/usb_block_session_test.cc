// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Link the production session and channel implementations. Only the physical
// media boundary and broker loop are fixtures; this program never opens media.
#include "../../compat/usb_storage/block_session.h"
#include "../../compat/usb_storage/block_broker_server.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
std::atomic<unsigned> workers_entered{0};
std::atomic<unsigned> workers_authorized{0};
std::atomic<unsigned> worker_wait_failures{0};
std::atomic<unsigned> media_opened{0};
std::atomic<unsigned> media_validated{0};
HANDLE workers_changed = nullptr;
}

namespace gesher::usb_storage {
DWORD HostDisk::Open(const std::wstring& path, Access access, std::unique_ptr<HostDisk>* disk) {
  if (path != L"isolated-session-fixture" || access != Access::kReadOnly || !disk || *disk)
    return ERROR_INVALID_PARAMETER;
  disk->reset(new HostDisk(INVALID_HANDLE_VALUE));
  ++media_opened;
  return ERROR_SUCCESS;
}
HostDisk::~HostDisk() = default;
DWORD HostDisk::Validate() { ++media_validated; return ERROR_SUCCESS; }

DWORD ServeBlockRequests(BlockChannel& channel, HostDisk&, const std::function<DWORD()>& authorize) {
  ++workers_entered;
  // Always acquire the actual session authorization lock, even if handoff
  // failed before the worker was scheduled. Joining this worker with that lock
  // held deterministically deadlocks, rather than depending on pipe timing.
  const DWORD result = authorize ? authorize() : ERROR_ACCESS_DENIED;
  if (result == ERROR_SUCCESS) ++workers_authorized;
  SetEvent(workers_changed);
  if (result != ERROR_SUCCESS) return result;
  // No further authorization call can cause a revocation after the barrier.
  // Keep the actual channel alive until revoked, as the real broker does;
  // returning here would let AttachChild prune a completed worker's channel.
  if (WaitForSingleObject(channel.revocation(), 25000) == WAIT_OBJECT_0) return ERROR_OPERATION_ABORTED;
  ++worker_wait_failures;
  return ERROR_TIMEOUT;
}
}  // namespace gesher::usb_storage

namespace {

bool AwaitAuthorized(unsigned expected, DWORD timeout = 3000) {
  const ULONGLONG deadline = GetTickCount64() + timeout;
  for (;;) {
    const unsigned observed = workers_authorized.load();
    if (observed >= expected) return observed == expected;
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline ||
        WaitForSingleObject(workers_changed, static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0)
      return false;
  }
}

// Handoff targets this isolated test process, so its narrowed guest handles
// really belong to this process and can be waited on and closed here.
struct LocalGuest {
  gesher::usb_storage::BlockGuestHandles handles;
  ~LocalGuest() {
    if (handles.pipe) CloseHandle(handles.pipe);
    if (handles.gate) CloseHandle(handles.gate);
    if (handles.revoked) CloseHandle(handles.revoked);
  }
  bool empty() const { return !handles.pipe && !handles.gate && !handles.revoked; }
  bool complete() const { return handles.pipe && handles.gate && handles.revoked; }
};

struct LocalObserver {
  HANDLE event = nullptr;
  ~LocalObserver() { if (event) CloseHandle(event); }
  bool CopyFrom(HANDLE process, const gesher::usb_storage::BlockGuestHandles& guest) {
    // The other handles, and the original event value, belong only to the
    // suspended child. Its eventual termination closes those remote copies.
    return guest.pipe && guest.gate && guest.revoked &&
        DuplicateHandle(process, guest.revoked, GetCurrentProcess(), &event, SYNCHRONIZE, FALSE, 0);
  }
};

struct SuspendedProcess {
  HANDLE job = nullptr;
  PROCESS_INFORMATION info{};
  ~SuspendedProcess() {
    if (info.hProcess && !Stop()) {
      std::fputs("Cannot terminate/reap owned suspended USB session fixture\n", stderr);
      ExitProcess(91);
    }
    if (info.hThread) CloseHandle(info.hThread);
    if (info.hProcess) CloseHandle(info.hProcess);
    if (job) CloseHandle(job);
  }
  bool Start() {
    std::array<wchar_t, 32768> image{};
    const DWORD length = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    if (length == 0 || length >= image.size()) return false;
    job = CreateJobObjectW(nullptr, nullptr);  // Non-inherited; only this test owns a job handle.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
      return false;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    if (!bytes) return false;
    std::vector<unsigned char> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) return false;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    // Only this test executable is created; it never executes a guest or even
    // its own entry point. Assign its kill-on-close job atomically at creation,
    // so even the watchdog's ExitProcess cannot orphan a suspended fixture.
    // There is deliberately no ResumeThread call.
    const bool created = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
        &job, sizeof(job), nullptr, nullptr) &&
        CreateProcessW(image.data(), nullptr, nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, nullptr, &startup.StartupInfo, &info);
    DeleteProcThreadAttributeList(attributes);
    BOOL contained = FALSE;
    return created && IsProcessInJob(info.hProcess, job, &contained) && contained;
  }
  bool Stop() {
    const DWORD state = WaitForSingleObject(info.hProcess, 0);
    if (state == WAIT_OBJECT_0) return true;
    if (state != WAIT_TIMEOUT || !TerminateProcess(info.hProcess, 0)) return false;
    return WaitForSingleObject(info.hProcess, 5000) == WAIT_OBJECT_0;
  }
};

int CheckExpiry(HANDLE process) {
  using namespace gesher::usb_storage;
  for (const ULONGLONG expired_at : std::array<ULONGLONG, 2>{0, GetTickCount64()}) {
    std::unique_ptr<BlockSession> expired;
    const unsigned opened = media_opened.load();
    const unsigned validated = media_validated.load();
    if (BlockSession::Open(L"isolated-session-fixture", Access::kReadOnly,
                          process, expired_at, &expired) != ERROR_INVALID_PARAMETER ||
        expired || media_opened != opened || media_validated != validated) return 9;

    std::unique_ptr<HostDisk> selected;
    if (HostDisk::Open(L"isolated-session-fixture", Access::kReadOnly, &selected)) return 10;
    if (BlockSession::Adopt(std::move(selected), process, expired_at, &expired) != ERROR_INVALID_PARAMETER ||
        selected || expired || media_validated != validated) return 11;
  }

  // A second, nonexpired session must remain live when the short grant expires.
  // Both sessions retain a real Windows process object; only their media and
  // broker callbacks are fixtures, not the clock, monitor or revocation event.
  const unsigned authorized = workers_authorized.load();
  std::unique_ptr<BlockSession> control;
  if (BlockSession::Open(L"isolated-session-fixture", Access::kReadOnly,
                        process, GetTickCount64() + 60000, &control)) return 12;
  LocalGuest control_guest;
  if (control->AttachChild(process, &control_guest.handles) != ERROR_SUCCESS ||
      !control_guest.complete()) return 13;

  std::unique_ptr<BlockSession> timed;
  // Leave ample admission time even on a busy runner. Waiting on the actual
  // event instead of sleeping establishes that Monitor performed the revoke.
  const ULONGLONG deadline = GetTickCount64() + 5000;
  if (BlockSession::Open(L"isolated-session-fixture", Access::kReadOnly,
                        process, deadline, &timed)) return 14;
  LocalGuest guest;
  if (timed->AttachChild(process, &guest.handles) != ERROR_SUCCESS || !guest.complete() ||
      !AwaitAuthorized(authorized + 2) ||
      timed->Validate() != ERROR_SUCCESS ||
      WaitForSingleObject(guest.handles.revoked, 0) != WAIT_TIMEOUT) return 15;
  // Every fixture authorize callback has finished successfully. During this
  // event-only wait, only the production monitor can notice the deadline.
  if (WaitForSingleObject(guest.handles.revoked, 10000) != WAIT_OBJECT_0 ||
      GetTickCount64() < deadline || timed->Validate() != ERROR_TIMEOUT) return 16;

  LocalGuest refused;
  if (timed->AttachChild(process, &refused.handles) != ERROR_TIMEOUT || !refused.empty()) return 17;
  if (control->Validate() != ERROR_SUCCESS ||
      WaitForSingleObject(control_guest.handles.revoked, 0) != WAIT_TIMEOUT) return 18;
  return 0;
}

int CheckParentExit(HANDLE process) {
  using namespace gesher::usb_storage;
  SuspendedProcess parent;
  if (!parent.Start()) return 20;
  const unsigned authorized = workers_authorized.load();
  std::unique_ptr<BlockSession> control;
  if (BlockSession::Open(L"isolated-session-fixture", Access::kReadOnly,
                        process, GetTickCount64() + 60000, &control)) return 21;
  LocalGuest control_guest;
  if (control->AttachChild(process, &control_guest.handles) || !control_guest.complete()) return 22;
  std::unique_ptr<BlockSession> session;
  if (BlockSession::Open(L"isolated-session-fixture", Access::kReadOnly,
                        parent.info.hProcess, GetTickCount64() + 60000, &session)) return 23;
  std::array<LocalGuest, 2> guests;
  for (auto& guest : guests) {
    if (session->AttachChild(process, &guest.handles) || !guest.complete()) return 24;
  }
  if (!AwaitAuthorized(authorized + 3) || session->Validate() || control->Validate()) return 25;
  for (const auto& guest : guests) {
    if (WaitForSingleObject(guest.handles.revoked, 0) != WAIT_TIMEOUT) return 26;
  }
  if (WaitForSingleObject(control_guest.handles.revoked, 0) != WAIT_TIMEOUT || !parent.Stop()) return 27;
  // No request invokes Validate until the monitor has signaled both channels.
  for (const auto& guest : guests) {
    if (WaitForSingleObject(guest.handles.revoked, 5000) != WAIT_OBJECT_0) return 28;
  }
  LocalGuest refused;
  if (session->Validate() != ERROR_PROCESS_ABORTED ||
      session->AttachChild(process, &refused.handles) != ERROR_PROCESS_ABORTED || !refused.empty()) return 29;
  if (control->Validate() || WaitForSingleObject(control_guest.handles.revoked, 0) != WAIT_TIMEOUT) return 30;
  return 0;
}

int CheckChildExit(HANDLE process) {
  using namespace gesher::usb_storage;
  SuspendedProcess ended, control;
  if (!ended.Start() || !control.Start()) return 40;
  const unsigned authorized = workers_authorized.load();
  std::unique_ptr<BlockSession> session;
  if (BlockSession::Open(L"isolated-session-fixture", Access::kReadOnly,
                        process, GetTickCount64() + 60000, &session)) return 41;
  BlockGuestHandles ended_guest, control_guest;
  if (session->AttachChild(ended.info.hProcess, &ended_guest) ||
      session->AttachChild(control.info.hProcess, &control_guest)) return 42;
  LocalObserver ended_observer, control_observer;
  if (!ended_observer.CopyFrom(ended.info.hProcess, ended_guest) ||
      !control_observer.CopyFrom(control.info.hProcess, control_guest)) return 43;
  if (!AwaitAuthorized(authorized + 2) || session->Validate() ||
      WaitForSingleObject(ended_observer.event, 0) != WAIT_TIMEOUT ||
      WaitForSingleObject(control_observer.event, 0) != WAIT_TIMEOUT) return 44;
  if (!ended.Stop() || WaitForSingleObject(ended_observer.event, 5000) != WAIT_OBJECT_0) return 45;
  // Ending one admitted child must not revoke its live sibling or its parent.
  if (session->Validate() || WaitForSingleObject(control.info.hProcess, 0) != WAIT_TIMEOUT ||
      WaitForSingleObject(control_observer.event, 0) != WAIT_TIMEOUT) return 46;
  return 0;
}

}  // namespace

int main() {
  using namespace gesher::usb_storage;
  HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!finished) return 1;
  workers_changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!workers_changed) { CloseHandle(finished); return 1; }
  std::thread watchdog([finished] {
    if (WaitForSingleObject(finished, 30000) != WAIT_OBJECT_0) {
      std::fputs("BlockSession cleanup, expiry or process-exit check exceeded its bounded wait\n", stderr);
      ExitProcess(90);
    }
  });
  int status = 0;
  {
    // This real process handle has enough access for identity retention but
    // cannot duplicate a data capability into the process. Handoff must fail.
    HANDLE reduced = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                 FALSE, GetCurrentProcessId());
    if (!reduced) status = 2;
    std::unique_ptr<BlockSession> session;
    if (!status && BlockSession::Open(L"isolated-session-fixture", Access::kReadOnly,
                                     reduced, GetTickCount64() + 60000, &session)) status = 3;
    for (unsigned attempt = 0; !status && attempt < 4; ++attempt) {
      BlockGuestHandles handles;
      const DWORD result = session->AttachChild(reduced, &handles);
      if (result != ERROR_ACCESS_DENIED || handles.pipe || handles.gate || handles.revoked) status = 4;
      if (workers_entered != attempt + 1 || workers_authorized != attempt + 1) status = 5;
      if (session->Validate() != ERROR_SUCCESS) status = 6;
    }
    if (session) {
      if (session->Revoke() || session->Validate() != ERROR_OPERATION_ABORTED) status = 7;
      session.reset();  // Joins the real monitor and every failed worker.
    }
    if (reduced) CloseHandle(reduced);
  }
  if (!status) {
    HANDLE process = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                 FALSE, GetCurrentProcessId());
    status = process ? CheckExpiry(process) : 8;
    if (!status) status = CheckParentExit(process);
    if (!status) status = CheckChildExit(process);
    if (process) CloseHandle(process);
  }
  if (!status && worker_wait_failures != 0) status = 60;
  SetEvent(finished);
  watchdog.join();
  CloseHandle(workers_changed);
  CloseHandle(finished);
  if (status) std::fprintf(stderr, "BlockSession contract regression failed at step %d\n", status);
  else std::puts("BlockSession failed-handoff cleanup, expired admission, actual clock expiry and parent/child exit revocation passed");
  return status;
}
