# arm64 translation (Phase 7)

Many Android apps ship their native code only for `arm64-v8a`. Gesher runs
x86-64 Android `.so` files natively (Phase 6, docs/native-libs.md); this track
runs **arm64-v8a** ones on x86-64 Windows by **translating** the guest AArch64
code, as Android's own native bridge (Houdini, Berberis) does. The decision and
the design are [ADR 0047](decisions/0047-arm64-translation.md).

## The pieces

- **The translator** is [dynarmic](https://github.com/yuzu-mirror/dynarmic)
  (0BSD; A64 frontend, x86-64 backend), pinned at commit
  `9d4582339990d4eae53f1dc7160686920fc2075c`. It lives in a **proxy DLL**,
  `gesher_arm64.dll` (`compat/arm64/dll/translator.cc`, `thunks.S`), whose one
  job is to run guest code and marshal the ABI. dalvikvm and the ELF loader do
  not link dynarmic; they `LoadLibrary` the DLL when they first meet an
  `EM_AARCH64` object (`compat/arm64/arm64_runtime.cc`). The interface is
  `compat/arm64/include/gesher/arm64_translator.h` (`gesher_arm64_init`).

- **The guest shares the host address space**: a guest `.so` is mapped like an
  x86-64 one and its virtual addresses are real host addresses, so the
  translator's memory accesses read and write host memory directly and no data
  is copied across the bridge. The host side built for Phase 6 (bionic shim,
  libandroid, EGL/GLES, the JNI proxies) is reused unchanged: the guest calls
  the very same System V entry points.

- **Guest → host**: each guest import is bound to a guest trampoline
  `SVC #n; RET`; the guest's call runs it, `CallSVC(n)` maps the guest's
  X0–X7/V0–V7 and stack arguments to System V (`gesher_call_host_sysv` in
  `thunks.S`), converting a `va_list` argument, and calls the host function.
  A host function the guest reaches as a plain pointer (dlsym, a host table)
  raises a no-execute fault in dynarmic and is called the same way. A few libc
  interfaces whose x86-64 layout differs get guest versions (`guest_libc.cc`).

- **Host → guest**: `host_entry(fn)` gives a System V x86-64 stub
  (`gesher_guest_entry_shim`) that enters the guest on the calling thread with a
  per-thread, per-nesting-depth `dynarmic::A64::Jit`, maps the System V
  arguments to AAPCS64, runs the guest until it returns to a sentinel X30, and
  returns X0/X1/V0. A host call straight to a guest pointer faults (guest code
  is not executable) and the vectored handler resumes it at that stub.
- **Faults** in guest code end the process with an Android-style
  `Fatal signal` report and exit status 128 + signal, never a hang.

- **The ELF loader** (`compat/elf`) maps `EM_AARCH64` objects with executable
  segments non-executable, applies the AArch64 relocations and TLS, and
  registers the code with the translator. x86-64 objects are unchanged.

## Building the DLL

`gesher_arm64.dll` is built by its own reusable workflow
(`.github/workflows/arm64.yml`) with the pinned llvm-mingw clang, like ANGLE's
DLLs (ADR 0018). The workflow clones dynarmic at the pinned commit and applies
two patches so its 2024-era vendored externals build with the 2026 llvm-mingw
clang (clang 23):

- **fmt**: `externals/fmt/include/fmt/base.h` — force `FMT_USE_CONSTEVAL 0`
  (fmt's `FMT_STRING` in its own sources trips a consteval-evaluation error on
  this clang); the bundled fmt is also replaced with 11.0.2, whose
  `fmt/ostream.h` does not include the removed libc++ `<__std_stream>` header.
- **mcl**: `externals/mcl/include/mcl/mp/typelist/lift_sequence.hpp` — the
  template-template parameter `template<class, T...> class` no longer matches
  `std::integer_sequence` on this clang; rewritten as
  `template<class VT, VT...> class`.

Boost headers (interval containers) are needed to build dynarmic; the workflow
fetches a pinned Boost. The DLL links the CRT and libunwind statically, so it
ships as a single file next to dalvikvm. Its license notice is installed as
`licenses/DYNARMIC-NOTICES.txt`.

## Status and next steps

_Last updated 2026-10-25 by the successor arm64 agent; branch `wip/arm64`._

### Review of the first implementation (this agent)

The first agent's ci `40196210581` hung both test jobs for hours: guest
store-exclusives always failed (dynarmic's default callbacks), so the first
guest atomic spun forever. Reproduced locally (the CI artifacts run on the
local machine; the DLL builds locally with llvm-mingw in a few minutes) and
fixed, with the other review findings: ADR 0047's "Revision" section lists
them all (crash path instead of re-run loops, DEP redirect to the host_entry
stub instead of running inside the VEH, xmm6-15 saved in the System V thunk,
stack arguments 7+, TCB slots, TLSDESC, JNI proxies per side, guest libc
differences, arch-filtered symbol binding, an honest overhead figure).

### Done and CI-verified (ci `40192217495`, by the first agent, both runners)

- `PASS arm64: translator DLL round-trips (host<->guest, dynarmic)`
- `PASS arm64: loader runs a self-contained arm64-v8a .so`
- `PASS arm64: System.load of an arm64-v8a .so; JNI_OnLoad, RegisterNatives,
  Java_* and callbacks into ART`

### Written, verified locally, CI pending (this push)

- The libc/pthread `.so` through the shim passes locally with the fixed DLL
  (qsort, threads, keys, once, cond vars, dl*, liblog).
- Guest crashes: `arm64_smoke_test crash-segv|crash-ill|crash-jump` print
  `Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0` (etc.) and
  exit 139/132 (locally).
- LL/SC atomics from four threads (smoke test, locally).
- `tests/native/arm64_abi_test.c`: ELF TLS via TLSDESC in several threads, the
  stack protector, setjmp/longjmp, struct stat/epoll_event, syscall numbers,
  AT_HWCAP, C11 atomics, va_list conversion, a host pointer from dlsym.
- The JNI test: Call*MethodV from a guest va_list, Java<->guest nesting 8
  deep, a guest pthread that AttachCurrentThread()s, calls Java, detaches.
- ABI selection: `NativeLibs.forApk` / `instructionSetDir`; the installer puts
  an arm64-only APK's libraries in `lib/arm64` and sets
  `ApplicationInfo.primaryCpuAbi`; gesher-app logs the ABI. Exercised by
  `nativeactivity-arm64.apk` (tests/apps/nativeactivity built with
  `--abi arm64-v8a`): NativeActivity, the glue thread, libandroid, EGL/GLES,
  AChoreographer, AInputQueue, ANativeWindow_lock, all translated, with the
  same checks as the x86_64 app.
- Perf: the smoke test's register-only loop is ~8.5x native (locally; the
  first agent's 1.3x compared against a native loop with a `volatile` store
  per iteration). `arm64_elf_test` also times a compiled C kernel (sieve,
  CRC-32, LCG) translated vs. the same code built natively.

- arm64 **executables** (the exec track, ADR 0035): gesher-exec loads an
  arm64-v8a PIE like an x86-64 one, sees `ExecutableInfo::aarch64` and enters
  its `_start` through the translator (`run_program`, API version 2) with the
  Linux initial stack block copied to the top of the thread's guest stack;
  bionic's `__libc_init`, `exit` and `getauxval` are gesher-exec's host
  functions, reached through trampolines. `exec_spawn` accepts EM_AARCH64, so
  ProcessBuilder runs arm64 programs too. Tested with the exec track's own test
  program built for arm64 (`arm64-exec/`), the same checks as
  tracks/42-exec.ps1.
- **Fastmem**: guest loads and stores are generated inline as host accesses
  at `[0 + vaddr]` (the shared address space), instead of a callback per
  access; `GESHER_ARM64_FASTMEM=0` turns it off. Exclusive accesses still go
  through the CAS callbacks.
- e2e: **TIC-80** 1.01.00 (`com.nesbox.tic`, F-Droid versionCode 10100,
  MIT), whose APK ships native code only for arm64-v8a (SDL2 and the console
  in C), in tests/e2e/apps.json; its log must show the translated library
  (`gesher arm64: <lib> is arm64-v8a: translated`, printed when
  System.loadLibrary loads an arm64 object).

### Not yet done

- Guest JIT code (RWX pages the guest writes), guest signal handlers with an
  arm64 `ucontext`, LSE atomics (dynarmic does not decode them; HWCAP hides
  them), HFAs by value.

### Dead ends ruled out

- **Berberis** (AOSP's native bridge): only riscv64 is implemented at
  android-16.0.0_r4; its arm64 guest would have to be written.
- **QEMU TCG user mode**: GPLv2, incompatible with ADR 0001.

### Local loop (no WSL, no AOSP)

Download a CI run's `windows-x64` and `arm64` artifacts into one directory,
build `gesher_arm64.dll` locally (llvm-mingw release for Windows, cmake and
ninja from pip, dynarmic/fmt/Boost fetched and patched as `arm64.yml` does),
copy it over, and run `arm64_smoke_test.exe` / `arm64_elf_test.exe <so> <dir>`.
Loader or shim changes still need CI (they are in the main build).
