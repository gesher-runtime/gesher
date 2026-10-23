# Roadmap

Each phase ends with something demonstrable, built and tested in CI.

| Phase | Goal | Done when |
|---|---|---|
| **0** | Research and infrastructure | CI cross-compiles with llvm-mingw and tests on Windows; Windows primitives for ART validated; porting surface measured |
| **1** | ART on Windows | `dalvikvm` runs a `Hello World` `.dex` from `cmd.exe` |
| **2** | Headless framework | an app's Activity goes through its lifecycle with resources and manifest loaded |
| **3** | Graphics and input | a simple Activity is drawn in a Win32 window via ANGLE and responds to clicks |
| **4** | First real apps | pure-Java apps work end to end; fonts, Hebrew/RTL; `.apk` file association |
| **5** | Network and web | TLS against the Windows cert store; `WebView` via WebView2 |
| **6** | Native x86-64 libraries | ELF loader and bionic-on-Win32 |
| **7** | arm64 translation | arm64-only apps run on x86-64 PCs |
| **8** | Everyday use | media, audio, notifications, microG, installer |

## Phase 0 checklist

- [x] License chosen ([0001](decisions/0001-license.md))
- [x] Toolchain chosen ([0002](decisions/0002-toolchain.md))
- [x] CI-first build strategy ([0003](decisions/0003-ci-first.md))
- [x] Platform probe: implicit null checks, stack overflow, futex, low-4 GiB
      heap, fixed-address file mapping, JIT
- [x] AOSP porting-surface survey, pinned to `android-16.0.0_r4`

## Phase 1 checklist

- [x] Build approach decided: CMake mirroring `Android.bp`, with overlays and
      patches ([0004](decisions/0004-build-system.md))
- [x] libbase, liblog, libziparchive, zlib, tinyxml2, fmtlib for Windows
- [x] libartpalette, libartbase, libdexfile for Windows
- [x] `dexdump.exe` reads and disassembles `.dex` files, including from a
      compressed `.apk`, tested in CI on Windows 2022 and 2025
- [x] gesher-base POSIX layer started ([0005](decisions/0005-posix-layer.md)):
      `mmap` family (anonymous maps, hints, partial unmap, `mprotect`,
      `madvise`), signal sets and handler registration; Linux-layout
      `pid_t`, `siginfo_t`, `ucontext_t`; headers for `dlfcn`, `elf`,
      `link`, `resource`, `uio`, `utsname`, `statvfs`, `poll`, `unistd`
- [x] LLP64: ART treats Windows x64 as 64-bit (patch 0002)
- [x] Generated sources outside Soong: aconfig flags, `asm_defines.h`
      (computed for the Windows target), enum operators
- [x] **ART runtime compiles for Windows**: all 237 files of libart-runtime,
      plus libelffile, libprofile, libnativebridge, libnativeloader, liblzma,
      lz4, cpu_features
- [x] Implement the declared POSIX functions (`dlopen`,
      `sysconf`, `getrlimit`, `poll`, `uname`, futex via `syscall`, ...)
- [x] **`dalvikvm.exe` links and starts** (`-showversion`), tested in CI on
      Windows 2022 and 2025. JIT compiler and native unwinding are stubbed
      for now (`compat/art/`)
- [x] Calling convention and thread register decided
      ([0006](decisions/0006-managed-abi.md)); gesher-base keeps `Thread*` in
      a TLS slot reachable through `%gs`
- [x] Assembly builds as COFF (nterp generated, `.type`/`.size`/`.hidden`/PLT
      handled); `Thread*` loads ported
- [x] Every thread-register access in the assembly (`%gs:FIELD`, nterp's
      `rSELF`) goes through the TLS slot; the `asm-port` CI job keeps it at 0
- [x] `sysv_abi` (`ART_SYSV_ABI`) on every C++ function the assembly calls
      and on the assembly C++ calls; C library calls go through System V
      wrappers. `tools/abi-check.py` verifies all 203 boundaries in CI
- [ ] Math entrypoints (`sin`, `fmod`, ...) called by compiled code need
      System V wrappers too (the Microsoft ABI's 32-byte shadow space);
      only matters once the JIT/AOT compiler runs
- [x] Natives entered with the System V ABI: `JNICALL` is `sysv_abi`, and
      `jni_macros.h` registers System V thunks; the `JNIEnv`/`JavaVM` tables
      stay on the Microsoft ABI ([0011](decisions/0011-libcore-natives.md))
- [x] Android root and data directories from the environment (patch 0008)
- [x] Boot class path built from libcore in CI: `core-oj`, `core-libart`
      ([0007](decisions/0007-boot-class-path.md))
- [x] Real signal chain (libsigchain) on gesher-base; ART uses explicit
      null, suspend and stack-overflow checks until signals are delivered
- [x] dlmalloc in its POSIX configuration, so the non-moving space stays in
      ART's low-4 GiB heap
- [x] `dalvikvm.exe` links the core classes from the boot jars without a boot
      image (tested in CI)
- [x] Crash reports: unhandled exceptions print a stack trace
      (`tools/symbolize.py`, or `addr2line` on an unstripped build);
      `GESHER_TRACE_MMAP=1` traces the mmap layer
- [x] Linux errno numbering, translated from the CRT's and winpthreads'
      ([0008](decisions/0008-linux-abi.md))
- [x] Sockets as file descriptors over Winsock, `poll` over files, pipes and
      sockets, with a probe run in CI ([0009](decisions/0009-socket-fds.md))
- [x] libcore's native libraries (`libicu_jni`, `libjavacore`,
      `libopenjdk`) linked in and registered with `dlopen` by name
      ([0011](decisions/0011-libcore-natives.md)), all three `JNI_OnLoad`s
      run (tested in CI)
- [x] ICU4C built from `external/icu`, its data mapped from
      `ANDROID_I18N_ROOT` as on Android; `libicu_jni`, libcore's ICU natives
      and `core-icu4j.jar` on the boot class path
- [ ] Time zone data (`ANDROID_TZDATA_ROOT`, libcore's `tzdata`)
- [ ] BoringSSL (`NativeBN`), epoll and inotify, a process model
- [x] Path model ([0010](decisions/0010-paths.md)): Linux paths are
      drive-less paths on the current drive, so ART's `:`-separated lists
      need no drive letters; `/dev/null`, `/dev/zero`, `/dev/urandom` and
      `/proc/self/cmdline` are emulated; `open` takes Linux's `O_*` flags
      over `CreateFileW`; tested in CI
- [ ] Paths on another drive in `:`-separated lists, and the rest of the
      CRT's path functions (`fopen`, `unlink`, `rename`, `access`)
- [x] Signals sent to the process wait for `sigwait`/`sigwaitinfo`, as
      ART's signal catcher takes them
- [ ] Signal delivery from Windows exceptions (VEH → `sigaction` handlers,
      `CONTEXT` ↔ `ucontext_t`), then implicit checks back on
- [ ] `MAP_FIXED` over existing reservations (placeholder mappings)
- [ ] Executable file mappings (`.oat` code)
- [ ] ART's own gtests for libartbase/libdexfile running on Windows
- [x] `dalvikvm.exe` runs `Hello` with the interpreter (tested in CI)
- [x] A trial installer from CI (`tools/package`, Inno Setup): per user, a
      `gesher` command for `.dex` programs, installed and tried on a clean
      runner

## Phase 2 checklist

Approach: [0012](decisions/0012-headless-framework.md); what framework.jar
needs: [framework-build.md](framework-build.md).

- [x] `framework-res.apk`, and its `R` and `Manifest`, built with aapt2
      (`tools/build-framework-res.sh`, with release BP4A's flag values)
- [x] The framework's dependency graph mapped (`tools/gen/bp_deps.py`)
- [x] aconfig flag classes as `java_aconfig_library` makes them
      (`Flags`, `FeatureFlags`, `FeatureFlagsImpl`)
- [x] Host tools built in CI: hidl-gen, protoc's javanano and javastream
      plugins, stats-log-api-gen, sysprop, xsdc, systemfeatures
      (`.github/workflows/framework.yml`, cached and shared across branches)
- [x] Code generation for the framework: AIDL, HIDL, protos, statslog,
      sysprop, XSD, event log tags
- [x] `framework.jar` compiles (javac, no errors) and is dexed (D8), in CI
- [x] `framework.jar` on the boot class path, tested on Windows (FrameworkSmoke)
- [x] `epoll` and `eventfd` in gesher-base, for `Looper`
      (`probes/posix/epoll_probe.cc`)
- [x] libutils, libcutils and libandroidfw in full, with AOSP's gtests on
      Windows (ci run 39956255117; exclusions in `docs/core-libs.md`)
- [x] `libandroid_runtime`'s host natives linked in: assets, resources,
      `Log`, `SystemProperties`, `MessageQueue`, `Parcel`, `Binder`
- [x] In-process stand-ins for the services the lifecycle calls
- [x] `gesher-app <app.apk>`: an `Activity` reaches `onResume` with its
      resources and manifest loaded (tested in CI, and through the installer)

## Phase 3 checklist

- [ ] Skia for Windows (CPU raster first), built in CI
- [ ] hwui's natives on Skia (Canvas, Paint, Path, Matrix, RenderNode,
      Bitmap) in place of the Phase 2 stand-ins
- [ ] ANGLE: EGL and GLES over Direct3D 11, built in CI
- [ ] A Win32 window as the Activity's surface (`ANativeWindow`), opened
      through a window manager stand-in
- [ ] A simple Activity drawn in a Win32 window, tested in CI by reading
      back its pixels (WARP)
- [ ] Input: Win32 mouse and keyboard messages delivered as `MotionEvent`
      and `KeyEvent`; a click reaches `onClick`, tested in CI
- [ ] Fonts through minikin (bundled or Windows fonts), Hebrew and RTL text

## Phase 4 checklist

- [x] Binder across processes: `/dev/binder` over Windows IPC, with
      `servicemanager`
- [ ] The framework's own system services (PackageManager parses the APK,
      ActivityManager starts the Activity) in place of gesher-app's stand-ins
- [x] `.apk` file association from the installer (per user; see
      `docs/installer.md`), tested in CI by the package job

## Phase 6 checklist

- [ ] ELF loader for x86-64 Android `.so` files on Windows
- [ ] bionic on Win32: libc, libm, libdl, liblog and the NDK's libandroid
      over gesher-base
- [ ] `System.loadLibrary` of an app's NDK library from its APK's
      `lib/x86_64`, with JNI calls both ways, tested in CI
