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
- [ ] `JNICALL` and the `JNIEnv`/`JavaVM` tables as System V (libnativehelper)
- [ ] Android root, boot class path: libcore (`core-oj`, `core-libart`) and
      its native libraries for Windows
- [ ] Signal delivery from Windows exceptions (VEH → `sigaction` handlers,
      `CONTEXT` ↔ `ucontext_t`), needed for implicit null checks
- [ ] `MAP_FIXED` over existing reservations (placeholder mappings)
- [ ] Executable file mappings (`.oat` code)
- [ ] ART's own gtests for libartbase/libdexfile running on Windows
- [ ] `dalvikvm.exe` runs `Hello` with the interpreter
