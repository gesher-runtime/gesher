# 0016 — libandroid and the rest of libc for an app's ELF libraries

**Status:** accepted, 2026-10-23 (native-libraries track)

## Context
An NDK library links `libandroid.so` for the platform's C APIs (assets,
configuration, loopers, sensors, tracing, storage, windows) and uses far more
of libc than the first test libraries did: directories, the `*at()` calls,
sockets, `poll`/`select`, signals, time, `std::filesystem` inside
`libc++_shared.so`. The bionic shim ([0014](0014-native-libs-fs-base.md),
[0015](0015-native-libs-jni-bridge.md)) provides libraries symbol by symbol
as System V entry points; it has no real `.so` files.

On Android `libandroid.so` is a thin layer over the framework's native code:
an `AAssetManager*` is the Java `AssetManager`'s native `AssetManager2`, an
`ALooper*` is libutils' `Looper*`, an `AConfiguration` is a
`ResTable_config`. Gesher runs all of these already (libandroidfw, the
framework's `android_util_AssetManager.cpp`, libutils' `Looper` over
gesher-base's epoll), compiled with the Microsoft ABI into dalvikvm.

## Decision
- **libandroid is part of the shim's symbol table**, not a separate ELF
  object: `compat/bionic/libandroid.cc` (an OBJECT library linked into
  dalvikvm, next to the framework's natives) adds its System V entry points
  with `AddBionicSymbol` from a static initializer. They call the same
  framework code Android's libandroid calls, with Android's object identities
  (`ALooper*` is `Looper*`), so the Java main Looper is the main thread's
  `ALooper`, and an fd added with `ALooper_addFd` is polled by the Java
  message loop. Callbacks the `.so` passes (`ALooper_callbackFunc`, OBB
  callbacks) are System V and are wrapped where Gesher's code calls them,
  setting the thread up for loaded code first. `AAssetManager_fromJava` gets
  the runtime's real `JNIEnv` behind the bridge's proxy (`RealJniEnv`).
- **Hardware the host does not have is absent, not faked:** no sensors, no
  OBB mounting (scanning works), tracing off. Graphics (`ANativeWindow`,
  `ANativeActivity`) are stand-ins that fail visibly until the graphics track
  provides surfaces; the interface they need is in `docs/native-libs.md`.
- **The shim converts bionic's LP64 ABI wherever it differs from
  gesher-base's**: structure layouts (`dirent`, `statvfs`, `addrinfo`,
  `struct tm`, `timeval`, `sigaction`, `fd_set`), constant spaces
  (`sysconf`/`pathconf` names, `AI_*`/`EAI_*`), and Linux semantics Windows
  lacks by default (an unlink that removes the name of an open file:
  POSIX delete dispositions, and `fopen` through gesher-base's `open`, which
  shares delete access).
- **Signals are emulated inside the shim**, separate from gesher-base's and
  ART's (libsigchain): handlers, masks and pending sets in user space,
  delivered synchronously to the calling thread or at another thread's next
  delivery point (mask changes, `sigsuspend`, the shim's sleeps and polls).
  Fault signals are not delivered to the `.so`'s handlers.
- **Unresolved data imports load as inaccessible placeholders** that name the
  symbol if touched, as unresolved functions already load as aborting stubs.

## Consequences
- An app's `libandroid` calls reach the same framework objects its Java code
  uses; no second asset or looper implementation.
- libandroid exists only where the framework's natives are (dalvikvm), not in
  `elf_load_test`; a `.so` loaded there gets the aborting stubs for it.
- A signal sent to a thread that is computing or blocked outside the shim is
  delayed until it reaches a delivery point; crash reporters' `SIGSEGV`
  handlers do not run.
