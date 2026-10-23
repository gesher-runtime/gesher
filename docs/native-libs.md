# Native x86-64 libraries (Phase 6)

Many Android apps ship native code as ELF `.so` files built by the NDK for
Linux, under `lib/x86_64` in the APK. Windows cannot load them (`LoadLibrary`
wants PE), so Gesher loads them itself: an **ELF loader** maps the shared
object and links it against a **bionic shim** (libc/libm/libdl/liblog and the
common NDK libraries) built on gesher-base, so `System.loadLibrary` and JNI
work both ways.

This is the native-libraries track; its decisions are
[0014](decisions/0014-native-libs-fs-base.md) (%fs/TLS) and the ADRs that
follow it. It builds in steps, each verified in CI
(`tracks/40-native-libs.ps1`, `.github/workflows/native-libs.yml`).

## Pieces

- **The ELF loader** (`compat/elf/`, `gesher/elf_loader.h`). For an ET_DYN
  x86-64 object it:
  - maps the `PT_LOAD` segments into a single reservation through
    gesher-base's `mmap`, copying their contents and setting each segment's
    protection with `mprotect` (file-backed fixed mappings would need
    placeholder mappings on Windows, which gesher-base does not do yet);
  - parses `PT_DYNAMIC`: symbol and string tables, GNU and SysV hashes,
    `RELA`/`JMPREL`/`RELR` relocations, init/fini arrays, `PT_TLS`;
  - applies relocations — `R_X86_64_RELATIVE`/`RELATIVE64`, `RELR`,
    `GLOB_DAT`, `JUMP_SLOT`, `64`, `IRELATIVE`, and the TLS relocations
    `DTPMOD64`/`DTPOFF64`/`TPOFF64`;
  - resolves `DT_NEEDED` against already-loaded objects, the APK's extracted
    `lib/x86_64` (search paths), or the bionic shim (which provides the
    standard libraries' symbols without a real `.so`);
  - runs `DT_INIT`/`DT_INIT_ARRAY`, and finds exports (`JNI_OnLoad`, `Java_*`)
    by GNU/SysV hash for the JNI layer.

  Loaded code is **System V ABI** ([0006](decisions/0006-managed-abi.md)), so
  the loader calls into it (init functions, IFUNC resolvers) through
  `sysv_abi` pointers, and the shim's entry points the `.so` calls are
  `sysv_abi` too.

- **TLS and `%fs`** ([0014](decisions/0014-native-libs-fs-base.md)). Each
  thread that runs loaded code gets a bionic variant-II TLS block, with `%fs`
  pointed at it by `WRFSBASE`. The `fsbase` probe proves on the CI runners that
  Windows allows and preserves this.

- **The bionic shim** (`compat/bionic/`, later steps): the libc/libm/libdl/
  liblog (and libandroid, libc++_shared) symbols an app `.so` imports,
  implemented as System V entry points over gesher-base, with bionic's
  Linux struct layouts ([0008](decisions/0008-linux-abi.md)).

- **The runtime hook** (later steps): the host `OpenNativeLibrary`/`dlopen`
  path so `System.loadLibrary`/`System.load` find the app's `.so` through the
  ELF loader, and `JNI_OnLoad`/`RegisterNatives`/`Java_*` work through it.

## Steps and status

1. **Loader on a self-contained object.** `tests/native/trivial.c` is built by
   the NDK with `-nostdlib` (no `DT_NEEDED`, no TLS, no stack guard);
   `elf_load_test` loads it, checks the constructor ran, and calls its exports.
   Exercises mapping, `RELATIVE`/`RELR` and the init array, and the `fsbase`
   probe decides the `%fs` approach. **(this step)**
2. The bionic shim: libc/libm/libdl/liblog symbols; a `.so` that uses
   `malloc`, `stdio`, `pthread`, `errno`.
3. The JNI path: `System.loadLibrary`, `JNI_OnLoad`, `RegisterNatives`,
   `Java_*`, and `JNIEnv` calls from the `.so` back into ART.
4. libc++_shared and a C++ exception; a test APK under `lib/x86_64`, run
   through gesher-app in CI.

## Building the test libraries

`.github/workflows/native-libs.yml` builds the `.so` files with a pinned
Android NDK (r27c, `27.2.12479018`) on the Linux runner and uploads them as the
`native-libs` artifact; the Windows `test` job loads them through
`elf_load_test` (and later through gesher-app).

## Status and next steps

_Last updated 2026-10-23 by the native-libraries track; branch
`wip/native-libs` off `phase3/base`._

### Done and CI-verified

- **Step 1 — loader on a self-contained object.** The ELF loader
  (`compat/elf/`), the `fsbase` probe and `elf_load_test` are built and the
  track checks (`tests/windows/tracks/40-native-libs.ps1`) run on Windows
  2022/2025. The NDK builds `trivial.so` with `-nostdlib`; the loader maps it,
  applies RELATIVE/RELR, runs its constructor and calls its exports; the
  `fsbase` probe proves `WRFSBASE` works and the `%fs` base survives
  scheduling. **Verification run: `39942420552`** (trigger
  `gh run view 39942420552 -R gesher-runtime/gesher`). The first attempt,
  `39941328906`, failed on a `-Werror` unused parameter, now fixed; the
  `native-libs` NDK-build job passed in both.
  - If `39942420552` shows a failing track line, read it from the `test` job's
    log and fix before building on step 1. The loader logic is syntax-checked
    with the local WinLibs `g++` (see below) but only CI runs it.

### In progress / half-done

- **Step 2 — the bionic shim** (`compat/bionic/`). Written and syntax-checked
  but **deliberately not in CMake yet**, so it cannot break step 1's CI:
  - `abi_types.h` — bionic's LP64 pthread/type layouts (fixed-width, since the
    `.so` has 64-bit `long` and mingw has 32-bit).
  - `pthread_bionic.cc` — bionic's pthread ABI over Windows threads and
    `WaitOnAddress` futexes (mutex/cond/once/keys/create/join); each created
    thread calls `elf::SetupThreadTls()` for `%fs`.
  - `shim.cc` — a System V symbol table (libc mem/str/stdio, libm, a core of
    unistd, pthread, liblog, libdl, `__stack_chk_*`, `__cxa_atexit/finalize`,
    `__tls_get_addr`) and `InstallBionicShim()`, which registers the resolver
    with the loader.
  - **Next concrete steps:** add `compat/bionic/bionic.cmake` (a `gesher_bionic`
    static lib) and `include()` it from `CMakeLists.txt`; link it into
    `elf_load_test` and call `InstallBionicShim()` there; add a `libc.so`-using
    test to `native-libs.yml` (NDK, *with* libc, pthread, malloc, non-variadic
    stdio) and a track check that loads it. Verify under llvm-mingw — the local
    `g++` check does not cover `posix_memalign`/`strdup`/`strnlen`/`aligned_alloc`
    signature differences, so expect a few CRT-availability fixes there.

### Next steps after that (not started)

- **Step 3 — JNI path.** Hook the host `OpenNativeLibrary`/`dlopen`
  (`art/libnativeloader/native_loader.cpp` `!ART_TARGET_ANDROID` branch, which
  already calls plain `dlopen`/`dlsym`) so `System.loadLibrary` routes a `.so`
  through the ELF loader, and `JNI_OnLoad`/`RegisterNatives`/`Java_*` resolve
  through `elf::Lookup`. `JNIEnv`/`JavaVM` tables are MS-ABI
  ([0006](decisions/0006-managed-abi.md)); calls from the `.so` into them must
  go MS-ABI while the `.so`'s own natives are entered System V — reuse the
  `jni-sysv` machinery (`tools/jni-sysv-patch.py`, `NATIVEHELPER_SYSV_JNI_ENTRY`).
  The minimal edit to `native_loader.cpp` must be noted in the report (shared
  file / other track's domain is `libnativeloader`? — it is ART, not another
  track's; confirm with the coordinator).
- **Step 4 — libc++_shared and a C++ exception; a test APK under `lib/x86_64`,
  run through gesher-app.** Open problem: C++ exception unwinding needs
  `dl_iterate_phdr` to report the loaded `.so`'s program headers (the unwinder
  in libc++_shared reads `PT_GNU_EH_FRAME` that way); gesher-base's
  `dl_iterate_phdr` currently reports nothing. The loader must register each
  loaded object's phdrs and gesher-base's `dl_iterate_phdr` must enumerate them.

### Open problems and decisions

- **%fs for bionic TLS** — decided in
  [0014](decisions/0014-native-libs-fs-base.md): use `WRFSBASE`, proven per-run
  by the `fsbase` probe. Fallback (patching `%fs` accesses) is specified but not
  implemented; only needed if a runner reports `FSGSBASE` off.
- **Variadic System V entry points** (`printf` family, `__android_log_print`):
  cannot be forwarded to the MS-ABI CRT, because `va_start` is not allowed in a
  `sysv_abi` function on Windows ([0011](decisions/0011-libcore-natives.md)).
  The shim offers non-variadic stdio/logging and a fixed-argument
  `__android_log_print`; a general sysv→ms vararg bridge (an assembly trampoline
  that rebuilds the register save area) is future work. Keep the test `.so`
  clear of `printf`/variadic logging until then.
- **Static-only TLS**: every load-time module gets a static (initial-exec)
  offset, so a `PT_TLS` module loaded after a thread starts is absent from that
  thread's block. Fine while all `.so`s load before app threads start; a
  growable per-thread DTV is the fix when that changes.

### Dead ends ruled out

- **Load-time patching of `%fs` accesses** (the `%gs` approach from
  [0006](decisions/0006-managed-abi.md)) was considered and set aside for
  `WRFSBASE`: the app `.so` is prebuilt and the `%fs` prefix sits on
  variable-length instructions, so in-place rewriting is far more invasive than
  one `WRFSBASE` per thread. Kept only as the documented fallback.
- **Forwarding bionic pthread to winpthreads**: impossible — bionic's
  `pthread_mutex_t` is 40 bytes with a different layout than winpthreads', and
  the `.so` allocates the bionic-sized object. The shim implements the ABI
  directly instead.
- **File-backed fixed segment mappings** for `PT_LOAD`: avoided; the loader
  reserves one anonymous range and copies segment contents, since file-backed
  `MAP_FIXED` over a reservation needs placeholder mappings gesher-base does not
  have yet.

### How to work on this

- Loader/shim logic can be syntax-checked locally without AOSP or WSL with the
  WinLibs UCRT `g++` already on the machine:
  `g++ -std=gnu++20 -fsyntax-only -Wall -Wextra -Werror -mfsgsbase -DGESHER
  -I compat/elf/include -I compat/bionic -I compat/posix/include <file.cc>`.
  It is not the CI toolchain (llvm-mingw) but catches most `-Werror` issues
  cheaply before a full CI build.
- Full verification is CI only: `gh workflow run ci --ref wip/native-libs`
  (the reusable `native-libs.yml` runs because `ci` calls it; it cannot be
  dispatched on its own until it reaches the default branch).
