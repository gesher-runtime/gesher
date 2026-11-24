// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Link the production session and channel implementations. Only the physical
// media boundary and broker loop are fixtures; this program never opens media.
#include "../../compat/usb_storage/block_session.h"
#include "../../compat/usb_storage/block_broker_server.h"

#include <atomic>
#include <cstdio>
#include <thread>

namespace {
std::atomic<unsigned> workers_entered{0};
std::atomic<unsigned> workers_authorized{0};
}

namespace gesher::usb_storage {
DWORD HostDisk::Open(const std::wstring& path, Access access, std::unique_ptr<HostDisk>* disk) {
  if (path != L"isolated-session-fixture" || access != Access::kReadOnly || !disk || *disk)
    return ERROR_INVALID_PARAMETER;
  disk->reset(new HostDisk(INVALID_HANDLE_VALUE));
  return ERROR_SUCCESS;
}
HostDisk::~HostDisk() = default;
DWORD HostDisk::Validate() { return ERROR_SUCCESS; }

DWORD ServeBlockRequests(BlockChannel&, HostDisk&, const std::function<DWORD()>& authorize) {
  ++workers_entered;
  // Always acquire the actual session authorization lock, even if handoff
  // failed before the worker was scheduled. Joining this worker with that lock
  // held deterministically deadlocks, rather than depending on pipe timing.
  const DWORD result = authorize ? authorize() : ERROR_ACCESS_DENIED;
  if (result == ERROR_SUCCESS) ++workers_authorized;
  return result;
}
}  // namespace gesher::usb_storage

int main() {
  using namespace gesher::usb_storage;
  HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!finished) return 1;
  std::thread watchdog([finished] {
    if (WaitForSingleObject(finished, 10000) != WAIT_OBJECT_0) {
      std::fputs("BlockSession failed handoff deadlocked during worker cleanup\n", stderr);
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
  SetEvent(finished);
  watchdog.join();
  CloseHandle(finished);
  if (status) std::fprintf(stderr, "BlockSession cleanup regression failed at step %d\n", status);
  else std::puts("BlockSession failed handoff releases authorization lock before joining its worker");
  return status;
}
