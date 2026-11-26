// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Interrupting a thread's futex wait for a signal directed at it.
//
// Linux interrupts a futex wait when a signal arrives for the waiting thread:
// the handler runs, and the wait returns EINTR (or restarts). The bionic shim
// (compat/bionic/signal_bionic.cc) keeps signals in user space, so it owns one
// gesher_futex_interrupt per thread and attaches it around the futex waits it
// makes for loaded code. Another thread that queues a signal for that thread
// requests an interrupt: an attached FUTEX_WAIT/FUTEX_WAIT_BITSET of the owner
// thread then ends with EINTR, before or while it blocks. Waits with no cell
// attached (gesher-base's and ART's own) are never interrupted.
//
// Like the futex queues, the attachment belongs to the executable's copy of
// this code (syscall.cc), whichever module calls these functions.
#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gesher_futex_interrupt {
  SRWLOCK lock;            // SRWLOCK_INIT
  volatile LONG requested; // an interrupt is requested and not yet consumed
  void* waiter;            // the owner thread's current wait, under `lock`
  const volatile void* address;  // or the word it waits on with WaitOnAddress
};
#define GESHER_FUTEX_INTERRUPT_INIT {SRWLOCK_INIT, 0, 0, 0}

// The calling thread's futex waits become interruptible through `cell`
// (nullptr: no longer). The cell must outlive the attachment.
void gesher_futex_interrupt_attach(struct gesher_futex_interrupt* cell);
// Requests an interrupt of `cell`'s owner thread: its attached futex wait in
// progress ends (EINTR), or its next one does at once. Async with respect to
// the owner; never blocks on it.
void gesher_futex_interrupt_request(struct gesher_futex_interrupt* cell);

#ifdef __cplusplus
}
#endif
