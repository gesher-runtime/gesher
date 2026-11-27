// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Real Windows thread/FLS teardown and the production fork exit gate. This
// does not clone a process or claim a reproduced loader-lock deadlock.
#include <windows.h>
#include <gesher/fork.h>
#include "../../compat/binder/loopback_relay.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

// A stand-alone build links the exact fork_locks.cc without the rest of ART.
// No cloned-child branch or diagnostic logger is exercised by this fixture.
#ifdef GESHER_RELAY_EXIT_TEST_BOUNDARIES
namespace gesher { bool InForkedChild() { return false; } }
namespace gesher::logging { void Record(int, int, const char*, const char*) {} }
#endif

namespace {
namespace relay = gesher::loopback_relay;
HANDLE opened, release_open, exiting, release_exit;
DWORD fls_index;
std::atomic<DWORD> worker_id{0};
std::atomic<bool> callback_timed_out{false}, captured_destroyed{false}, cleanup_before_gate{false};
struct Captured { ~Captured() { captured_destroyed.store(true); } };

void NTAPI ExitCallback(void*) {
  SetEvent(exiting);
  if (WaitForSingleObject(release_exit, 15000) != WAIT_OBJECT_0) callback_timed_out.store(true);
}
void EnterExitGate() {
  cleanup_before_gate.store(captured_destroyed.load());
  gesher::ThreadLeaving();
}
bool ForkReady(void*) { return relay::TryLockForkGate(); }
void ForkUnlock(void*) { relay::UnlockForkGate(); }
void ForkChild(void*) { relay::ResetForkGateAfterChild(); }
void* ForkIdentity() { static int identity; return &identity; }
}

int main(int argc, char** argv) {
  const bool omit_gate = argc == 2 && std::strcmp(argv[1], "--omit-gate") == 0;
  if (argc != 1 && !omit_gate) return 2;
  const gesher::ForkLockOps operations{ForkReady, ForkUnlock, ForkChild};
  gesher::RegisterForkLock(gesher::kForkOrderLeaf, "relay exit fixture", ForkIdentity, &operations);
  HANDLE* events[] = {&opened, &release_open, &exiting, &release_exit};
  for (HANDLE* event : events) {
    *event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!*event) return 3;
  }
  fls_index = FlsAlloc(ExitCallback);
  if (fls_index == FLS_OUT_OF_INDEXES) return 4;
  auto captured = std::make_shared<Captured>();
  auto client = relay::Client::Start({}, [captured](const relay::Request&) {
    worker_id.store(GetCurrentThreadId());
    if (!FlsSetValue(fls_index, reinterpret_cast<void*>(1))) return INVALID_HANDLE_VALUE;
    SetEvent(opened);
    WaitForSingleObject(release_open, 10000);
    return INVALID_HANDLE_VALUE;
  }, [captured] {}, omit_gate ? nullptr : EnterExitGate);
  if (WaitForSingleObject(opened, 5000) != WAIT_OBJECT_0) return 5;
  captured.reset();
  client.reset();
  SetEvent(release_open);
  if (WaitForSingleObject(exiting, 5000) != WAIT_OBJECT_0) return 6;
  HANDLE worker = OpenThread(SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, FALSE, worker_id.load());
  const bool held_callback = worker && WaitForSingleObject(worker, 0) == WAIT_TIMEOUT;
  const bool counters_retired = !relay::HasProcessLocalState();
  // WaitForLeavingThreads uses its real bounded wait. The callback remains
  // held until this returns; a fixed gate must refuse this snapshot.
  const int acquired = gesher::AcquireForkLocks(nullptr, nullptr, 50);
  if (acquired == 0) gesher::ReleaseForkLocks(false);
  const bool callback_still_held = worker && WaitForSingleObject(worker, 0) == WAIT_TIMEOUT;
  SetEvent(release_exit);
  const bool finished = worker && WaitForSingleObject(worker, 5000) == WAIT_OBJECT_0;
  if (worker) CloseHandle(worker);
  const bool idle_snapshot = gesher::AcquireForkLocks(nullptr, nullptr, 50) == 0;
  if (idle_snapshot) gesher::ReleaseForkLocks(false);
  const bool released_fls = FlsFree(fls_index) != FALSE;
  bool closed_events = true;
  for (HANDLE* event : events) closed_events &= CloseHandle(*event) != FALSE;
  const bool cleanup = finished && idle_snapshot && released_fls && closed_events &&
      !callback_timed_out.load() && !relay::HasProcessLocalState();
  std::printf("RELAY_EXIT_REAL held_fls=%d counters_retired=%d fork_result=%d "
      "callback_still_held=%d captured_destroyed=%d cleanup_before_gate=%d cleanup=%d\n",
      held_callback, counters_retired, acquired, callback_still_held,
      captured_destroyed.load(), cleanup_before_gate.load(), cleanup);
  if (!held_callback || !counters_retired || !callback_still_held || !cleanup) return 7;
  if (acquired == 0) {
    std::fputs("FAIL relay exit: fork accepted while real FLS exit callback was active\n", stderr);
    return 1;
  }
  if (!cleanup_before_gate.load()) return 8;
  std::puts("PASS relay exit: real FLS callback excludes fork until actual thread exit");
  return 0;
}
