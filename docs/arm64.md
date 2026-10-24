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
  `SVC #0; RET`; the guest's call runs it, `CallSVC` maps the guest's
  X0–X7/V0–V7 to System V (`gesher_call_host_sysv` in `thunks.S`) and calls the
  host function.

- **Host → guest**: `host_entry(fn)` gives a System V x86-64 stub
  (`gesher_guest_entry_shim`) that enters the guest on the calling thread with a
  per-thread `dynarmic::A64::Jit`, maps the System V arguments to AAPCS64, runs
  the guest until it returns to a sentinel X30, and returns X0/V0.

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

_Last updated 2026-10-25 by the arm64 track; branch `wip/arm64`._

### Done

- **ADR 0047**: dynarmic chosen (Berberis is riscv64-only at the pinned tag;
  QEMU is GPL); the proxy-DLL, shared-address-space, trampoline design.
- The translator interface (`gesher/arm64_translator.h`), the DLL
  (`translator.cc`, `thunks.S`), its standalone CMake and a smoke test
  (`arm64_smoke_test.cc`).

### In progress / not yet CI-verified

- Building `gesher_arm64.dll` with llvm-mingw and the step-1 smoke test
  (host↔guest round trips) in `.github/workflows/arm64.yml` and
  `tracks/47-arm64.ps1`.
- EM_AARCH64 support in `compat/elf` (relocations, TLS), the loader glue in
  `compat/arm64`, binding guest imports to trampolines, and loading a guest
  `.so` built by the NDK for `arm64-v8a` that calls libc.
- JNI both ways, libandroid, EGL/GLES, NativeActivity for guest `.so` files.
- ABI selection: `arm64-v8a` chosen when an APK has no x86_64 libs;
  `Build.SUPPORTED_ABIS` lists it after x86_64; `ro.dalvik.vm.native.bridge`.
- Tests: the arm64-only test APKs, a real arm64-only F-Droid app in the e2e
  job, and the translation-overhead measurement.

### Dead ends ruled out

- **Berberis** (AOSP's native bridge): only riscv64 is implemented at
  android-16.0.0_r4; its arm64 guest would have to be written.
- **QEMU TCG user mode**: GPLv2, incompatible with ADR 0001.
