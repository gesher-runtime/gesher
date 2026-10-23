# 0015 — Binder across processes: /dev/binder over a broker process

**Status:** accepted, 2026-10-23 (services track)

## Context
libbinder (built in-process in Phase 2, [0013](0013-framework-natives.md))
talks to the kernel binder driver: `open("/dev/binder")`, an `mmap` receive
buffer, and `BINDER_WRITE_READ` ioctls whose command streams carry
transactions, replies, reference counts and death notifications. The driver
decides which thread receives a transaction (nested calls go back to the
waiting thread), translates objects between processes (local pointer ↔
handle), counts references and tells owners (`BR_INCREFS`/`BR_ACQUIRE`/...),
and tells holders when a process dies. Windows has nothing like it; the
framework (servicemanager, system services, apps) needs it unchanged.

## Decision
- **A broker process plays the driver**: `binderd.exe`
  (`compat/binder/binderd.cc`) keeps the driver's state (procs, threads,
  nodes, refs, transactions, buffers) and follows `drivers/android/binder.c`'s
  rules function by function (`binder_transaction`, `binder_thread_read`,
  `binder_inc_node`, `binder_node_release`, `binder_send_failed_reply`...),
  so libbinder needs no change to its IPC code.
- **Named pipes, one connection per binder thread.** gesher-base's
  `/dev/binder` (`compat/posix/src/binder.cc`) opens a process connection at
  `open()` (its breaking is process death: `binder_release`) and one
  connection per thread on its first ioctl (closed by a fiber-local callback
  when the thread exits: `binder_thread_release`). Each request gets one
  reply; the broker's thread for a connection blocks where the kernel would,
  on a condition variable under one global lock. Wire format:
  `compat/posix/src/binder_wire.h`.
- **No shared memory.** The broker cannot read the sender's memory, so the
  client sends transaction data inline after each `BC_TRANSACTION`/`BC_REPLY`,
  and copies each delivered transaction into the process's binder mapping (the
  fd's `mmap`, an anonymous region binder.cc allocates in), patching the
  pointers, so libbinder sees kernel-like buffers; `BC_FREE_BUFFER`'s pointer
  is mapped back to the broker's buffer id.
- **The device is opt-in**: `$GESHER_BINDER` names it
  (`\\.\pipe\gesher-binder-<name>`). Without it, or with no binderd serving it,
  `/dev/binder` does not exist (ENOENT) and libbinder stays in-process as in
  Phase 2, so gesher-app's single-process mode is unchanged.
- **servicemanager is frameworks/native's**, built as vndservicemanager
  (`VENDORSERVICEMANAGER`: no VINTF, no perfetto), Access off-Android (allow
  all), with `compat/binder/servicemanager_main.cc` joining the thread pool
  instead of polling the binder fd (binderd does not support poll; lazy
  service client callbacks are therefore not sent).
- frameworks/native patch 0002: libbinder's `BackendUnifiedServiceManager`
  (what Java's `ServiceManagerProxy` goes through) uses the out-of-process
  servicemanager when `$GESHER_BINDER` is set (upstream disables it off
  bionic), opening the driver with `ProcessState::self()` since gesher-base's
  `access()` is the CRT's.

## Not done
File descriptors in transactions (`BINDER_TYPE_FD`/`FDA`; plan: the broker
`DuplicateHandle`s between processes), scatter-gather (`BINDER_TYPE_PTR`,
HIDL only), `BR_TRANSACTION_SEC_CTX`, freeze, priority inheritance, poll on
the binder fd. Such transactions fail with `BR_FAILED_REPLY`.

## Consequences
- Every binder call is two pipe round trips' worth of latency (request and
  reply on each side); fine for lifecycle traffic, to be measured later.
- binderd must be started before the processes that use the device (tests
  start it; a launcher will).
