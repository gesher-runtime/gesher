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

- **Executables** (the exec track, ADR 0035): gesher-exec loads an arm64-v8a
  PIE like an x86-64 one, sees `ExecutableInfo::aarch64` and enters its
  `_start` through the translator (`run_program`, API version 2) with the
  Linux initial stack block copied to the top of the guest stack; bionic's
  `__libc_init`, `exit` and `getauxval` are gesher-exec's own, reached through
  trampolines. `exec_spawn` accepts EM_AARCH64, so ProcessBuilder runs arm64
  programs too.

- **Fastmem**: guest loads and stores are generated inline as host accesses
  at `[0 + vaddr]` (the shared address space) rather than a callback each;
  `GESHER_ARM64_FASTMEM=0` turns it off. Exclusives go through the CAS
  callbacks.

- **ABI selection**: `NativeLibs.abiFor` picks x86_64, else arm64-v8a; the
  installer extracts to `lib/arm64` and sets `primaryCpuAbi`; gesher-app logs
  the ABI; System.loadLibrary of an arm64 object logs
  `gesher arm64: <lib> is arm64-v8a: translated`.

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

## Performance

Measured on the CI runners (windows-2022/2025, 4 cores), printed as
`INFO  arm64 perf` by `tracks/47-arm64.ps1`:

| Workload | Translated | Native | Overhead |
|---|---|---|---|
| Register-only loop, 2·10⁸ iterations (smoke test, hand-written A64) | ~310 ms | ~62 ms | ×5.0 |
| Compiled C kernel: sieve, table CRC-32, LCG (`arm64_perf_kernel.h`, NDK -O2 vs. llvm-mingw -O2), 100 rounds | ~95 ms | ~26 ms | ×3.6 |

(ci `40213822267`, before fastmem; the first agent's ×1.3–1.6 compared against
a native loop with a `volatile` store per iteration.) dynarmic keeps guest
registers in memory between blocks and materialises NZCV, which a tight
compare-and-branch loop pays for on every iteration; compiled code with real
memory traffic fares better. The time includes translating each block once
(dynarmic caches translations per Jit). Fastmem (inline guest memory
accesses) took the kernel from ~×4.5 to ~×4.0 locally.

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

### Done and CI-verified (ci `40217845117` at 25683fc and ci `40237320424` at ef57e7d, both runners)

`tracks/47-arm64.ps1`, every line PASS on windows-2022 and windows-2025:

- `arm64: translator DLL round-trips (host<->guest, dynarmic)` (with LL/SC
  atomics from four threads)
- `arm64: a guest null dereference is a SIGSEGV crash report`, `... an
  undefined guest instruction is a SIGILL crash report`, `... a guest jump to
  no code is a SIGSEGV crash report`
- `arm64: loader runs a self-contained arm64-v8a .so`
- `arm64: an arm64-v8a libc/pthread .so runs its checks through the shim`
- `arm64: guest ABI (TLS, stack protector, setjmp, stat, epoll, syscall,
  atomics, va_list)`
- `arm64: System.load of an arm64-v8a .so; JNI both ways, Call*MethodV,
  nesting, an attached guest thread`
- `arm64: gesher-exec runs an arm64-v8a ELF executable (...)`, its exit
  statuses (3, 134, 139), and `ProcessBuilder runs an arm64-v8a ELF
  executable`
- `native libs: arm64: gesher-app runs nativeactivity-arm64.apk (only
  lib/arm64-v8a, translated): the ABI picked, ANativeActivity_onCreate, the
  glue thread, GLES 2 (glReadPixels, PrintWindow), AChoreographer, a click and
  a key through AInputQueue, an ANativeWindow_lock frame; WM_CLOSE finishes it`
- `INFO arm64 perf`: register loop ×3.7–4.2, compiled kernel ×2.0–2.3 (fastmem).

The arm64 workflow alone (run `40215298386`) ran the smoke test on both
runners too.

### e2e: TIC-80 (known failure, not an arm64 one)

TIC-80 1.01.00 (`com.nesbox.tic`, F-Droid versionCode 10100, MIT; its APK
ships native code only for arm64-v8a) is in tests/e2e/apps.json. In ci
`40237320424`: `libtic80.so` loads translated (`gesher arm64: libtic80.so is
arm64-v8a: translated`), SDL's JNI_OnLoad and its three `nativeSetupJNI`
natives run and call into ART, the window opens and the app closes cleanly
(exit code 0). It stays black because SDL draws into an `SDLSurface`, a
SurfaceView, which Gesher's graphics stack does not give a Surface yet, so
SDL's main thread never starts; listed as `knownFailure` for that reason.
On the way there, the DLL gained fallbacks for ~50 libc/libm functions the
shim lacks (sem_*, 32-bit wchar_t functions, acosh/erf/lgamma..., div,
getrusage, sched params), bound only when the shim has no such name.

### Not yet done

- A real arm64-only app reaching its first frame in e2e: TIC-80 needs
  SurfaceView (graphics track); the shim gaps above should move into the
  shim (compat/bionic) so x86_64 apps get them too.
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
