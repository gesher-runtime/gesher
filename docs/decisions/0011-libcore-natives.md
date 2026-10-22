# 0011 — libcore's native libraries, linked in

**Status:** accepted, 2026-10-21

## Context
At startup ART loads `libicu_jni.so`, `libjavacore.so` and `libopenjdk.so`
with `dlopen` and calls each one's `JNI_OnLoad`; failing that is fatal
(`runtime.cc`). Gesher builds one static executable per tool
([0004](0004-build-system.md)), and Android's ELF loader is not part of it.

## Decision
- **Linked in, and registered by name.** `cmake/aosp/libcore_native.cmake`
  builds the libraries from the file lists in libcore's `.bp` files as
  object libraries and links them into `dalvikvm`, and
  `cmake/aosp/icu.cmake` does the same for `libicu_jni`. Each library's
  `JNI_OnLoad` is compiled under its own name (`JNI_OnLoad_icu_jni`,
  `JNI_OnLoad_javacore`, `JNI_OnLoad_openjdk`), the JNI convention for
  statically linked libraries; names the libraries share (`JniConstants`,
  `maybeThrowIcuException`) are renamed in all but one.
  `compat/libcore/builtins.cc` registers them with gesher-base's `dlopen`
  (`<gesher/builtin_library.h>`): opening a registered name, with or
  without a directory, returns a handle whose `dlsym` looks in the
  library's table only, as `JNI_OnLoad` is not unique among the libraries
  linked in. `JNIEXPORT` exports nothing (libnativehelper patch 0001): the
  natives are registered, and Windows' `dllexport` clashes with
  declarations made without it.
- **OpenJDK's C sources see Linux.** They choose their platform code with
  `__linux__` and `WIN32`; gesher-base provides Linux's interfaces, so they
  are compiled with `__linux__` defined and `WIN32` undefined. C only:
  libc++'s headers test `__linux__` too.
- **ICU as on Android.** ICU4C (`libicuuc`, `libicui18n`) is built from
  `external/icu` for ICU's MinGW platform, so its own platform code is
  Win32's. Its data is the prebuilt `icudt76l.dat`, which libandroidicuinit
  maps from `$ANDROID_I18N_ROOT/etc/icu` through gesher-base's `open` and
  `mmap`, as on the ART host (icu patch 0001 enables that under `GESHER`).
  libcore compiles against the NDK's `libicu` headers, with `UCONFIG_USE_LOCAL`
  standing in for the platform build's `ANDROID` (which would switch on
  Android-only code elsewhere), and links its shim over ICU4C; `libicu_jni`
  uses ICU4C directly. ICU4J (`core-icu4j.jar`, with the libcore bridge
  whose classes `libicu_jni` registers natives on) joins the boot class path
  ([0007](0007-boot-class-path.md)) and finds the same data file.
- **Not ported yet, and said so.** Sources that need what Gesher does not
  have yet are left out, listed in `GESHER_LIBCORE_UNPORTED` and
  `GESHER_OPENJDK_UNPORTED`: `NativeBN` (BoringSSL), `EPoll` and
  `LinuxWatchService` (epoll, inotify), `UNIXProcess` (`vfork`).
  `compat/libcore/unported.cc` defines their registration functions, which
  register nothing, for each file still on the lists; calling one of their
  natives throws `UnsatisfiedLinkError`.
- **Natives are entered with the System V ABI**, as ART's trampolines call
  them ([0006](0006-managed-abi.md)). `JNICALL` is `sysv_abi`
  (libnativehelper 0002), which covers natives found by name
  (`Java_…`). Natives registered through `jni_macros.h`'s
  `NATIVE_METHOD` and friends are mostly C++ functions declared without
  `JNICALL`; the macros register a System V thunk generated for each one
  (`GesherSysvJni<F>::Call`) instead of the function itself. C sources and
  hand-written tables get `JNICALL` where they lack it (libcore 0001, art
  0010 for the interpreter's own calls of `JNI_OnLoad`-style pointers);
  sources that declare `JNICALL` natives and register them through the
  macros lose it, as the macros' signature check rejects `sysv_abi` types
  (libcore 0001 for `VM_getNanoTimeAdjustment`, 0002 for `Character`).
  Variadic helpers (`JNU_CallMethodByName` and the like) stay on the
  Microsoft ABI: `va_start` is not allowed in a `sysv_abi` function on
  Windows, and they are called from C, not from ART. The `JNIEnv` and
  `JavaVM` function tables, and `JNI_OnLoad`, are called and implemented by
  code built by the same compiler, so they stay on the Microsoft ABI too.
- **Patches** where the sources assume LP64 or Linux-only names: libcore
  0001 (64-bit `jlong_to_ptr` and buffer sizes on Windows x64, `mmap`'s
  address not cast through `long`, no OpenSSL version, `JNICALL`s), art 0010
  (`gesher_gethostname`, `JNICALL`s), libnativehelper 0001 (`JNIEXPORT`)
  and 0002 (`JNICALL`, the thunks), and libbase 0002 (libbase's `open`
  takes Linux's `O_*` flags).
- **Found on the way**, once Java code ran and allocated: the CMC
  collector without userfaultfd moves its space aside with `mremap`, which
  gesher-base now does by copying (`mman.cc`); and ART's `InlinedVector`
  left its size uninitialized (art 0011), which Linux's zeroed stack hid and
  which made `Class.getDeclaredFields` read garbage on Windows. Once `main`
  ran, the signal catcher's `sigwaitinfo` needed signals sent to the process
  to wait for it (gesher-base's `signal.cc`).

## Known gaps
- The libraries above, and every native that needs them, until BoringSSL,
  epoll and inotify equivalents and a process model are there.
- Time zone data: `ANDROID_TZDATA_ROOT` has no `tzdata` yet, so ICU uses
  only what `icudt76l.dat` carries, and libcore's `ZoneInfoDb` has nothing.
- `Selector.open()` needs a selector without epoll; `WatchService` has none.
- LLP64: casts through `long` that do not truncate pointers but may
  truncate values (sizes, offsets) are not audited yet; the compiler only
  flags pointer truncation.
