# 0014 — libutils, libcutils and libandroidfw in full, tested by AOSP's tests

**Status:** accepted, 2026-10-23

## Context
Phase 2 built the core libraries' Windows source sets, with Looper and the
Unix sockets added, as much as the framework's first natives needed
([0013](0013-framework-natives.md)). The graphics, input, Binder and service
work ahead uses the rest: libutils' `Thread`, `Mutex`, `Condition` and
`RWLock`, libcutils' `str_parms`, `fs_config`, `multiuser` and hashmap,
libandroidfw's `CursorWindow` and PNG code. Soong builds none of it for
Windows, and nothing showed that what was built behaves as on a device.

## Decision
- **The device's source sets.** Each library builds every source its
  Android.bp builds for the device (the common, `linux`, `not_windows` and
  `android` sets), keeping its target name and include directories. Where the
  device's file drives a Linux kernel interface the host's file is built
  instead, as Soong does for Linux hosts: libcutils' `ashmem-host.cpp` (the
  device's talks to the ashmem driver or seals memfds and reads their ashmem
  ioctls) and `trace-host.cpp` (the device's writes to tracefs's
  `trace_marker`). libutilscallstack, disabled for Windows (libunwindstack
  reads ELF), builds over Gesher's own `AndroidUnwinder`.
- **libutils takes its pthread paths.** gesher-base implements POSIX threads
  (winpthreads plus its additions), so under `GESHER` libutils' headers and
  `Threads.cpp` take the branches Linux takes (system/core patch 0003):
  `Mutex`, `Condition`, `RWLock` and `Thread` are the device's classes, not
  the Win32 `Mutex` and `Condition` stand-ins (and no `Thread` or `RWLock`)
  Windows gets in AOSP. `android_thread_id_t` becomes the device's `void*`.
  Platform branches that pick a clock or an Android-only API (`timedLock`,
  thread priorities) keep their host branch.
- **Missing POSIX goes into gesher-base,** not around it: `select`/`pselect`,
  `fnmatch`, `random`/`srandom`, `bzero`, `<linux/capability.h>`, the
  `CPU_*` set operations, `NETLINK_KOBJECT_UEVENT`.
- **A Windows unwinder behind libunwindstack's interface.**
  `compat/unwindstack/android_unwinder.cc` implements `AndroidUnwinder` and
  `AndroidLocalUnwinder` over `RtlVirtualUnwind` (the calling thread from
  `RtlCaptureContext`, another from `GetThreadContext` while it is
  suspended), naming functions from each module's COFF symbol table, or its
  exports, and formats frames as libunwindstack does.
- **AOSP's own tests, in CI, on Windows.** googletest and googlemock build
  with the project's toolchain; the libraries' `cc_test` modules
  (libutils_test, libutils_binder_test and its custom-allocator variant,
  libcutils_test, libandroidfw_tests) build in `.github/workflows/libs.yml`
  with every source the device's test lists, and run on both Windows runners
  through `tests/windows/tracks/20-libs.ps1`. A test that cannot run on
  Windows is left out by name, with its reason, in
  [core-libs.md](../core-libs.md); test sources change only where Windows
  differs in a way the test does not depend on (patches listed there).

## Consequences
- Code built on the libraries gets the device's threading classes; their
  layout differs from AOSP's Windows build, which nothing Gesher builds uses.
- `compat/art/unwindstack_stub.cc` and `compat/unwindstack/` both define
  `AndroidLocalUnwinder`'s virtual functions; an executable may link one of
  them. ART can move to the real unwinder (its thread dumps then show native
  frames).
- libandroidfw links libbinder (`CursorWindow`'s `Parcel`) and libpng.
