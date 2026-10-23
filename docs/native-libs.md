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
  thread that runs loaded code gets a bionic variant-II TLS block
  (`compat/elf/tls.cc`). Pointing `%fs` at it with `WRFSBASE` was the plan,
  but the `fsbase` probe showed Windows does **not** preserve the user `%fs`
  base across kernel entry, so this is still open (see "Status and next
  steps").

- **The bionic shim** (`compat/bionic/`, CMake target `gesher_bionic`,
  `gesher/bionic_shim.h`): the libc/libm/libdl/liblog/pthread symbols an app
  `.so` imports, as System V entry points over gesher-base and the CRT, with
  bionic's LP64 types and Linux struct layouts
  ([0008](decisions/0008-linux-abi.md)). `InstallBionicShim()` registers it
  with the loader.
  - `shim.cc`: memory (CRT malloc; over-aligned blocks from
    `posix_memalign`/`memalign` are tracked so `free`/`realloc` accept them),
    strings, FORTIFY `__*_chk`, ctype, `long`-returning functions widened to
    64 bits (`strtol`, `lseek`, `sysconf`, `lround`...), `__errno` (the CRT's
    errno, which gesher-base keeps in Linux numbering), time (`clock_gettime`
    with Linux clock ids and a 64-bit `tv_nsec`), fd-level files, `stat`
    (gesher-base's Linux-layout struct with widened `tv_nsec`), `mmap`, libm,
    `qsort`/`bsearch` with a System V comparator, libdl over the loader
    (`dlopen`, `dlsym`, `dladdr`, `dl_iterate_phdr`), `__cxa_atexit`/
    `__cxa_finalize`, `__stack_chk_fail`, `__tls_get_addr`.
  - `stdio_bionic.cc`: `FILE` functions (also mapping API < 23's `__sF`), and
    the **variadic** ones — `printf`, `fprintf`, `snprintf`, `sprintf`,
    `asprintf`, `dprintf`, `sscanf`, `syslog`, `__android_log_print`,
    `__android_log_assert`, `__sprintf_chk`, `__snprintf_chk` — through an
    assembly trampoline per function that spills the argument registers into
    a System V register save area, builds a System V `va_list` over it and
    calls the `va_list` form. The formatter takes each argument from that
    `va_list` by its conversion and formats one conversion at a time with the
    CRT (`%ld` is 64-bit, `%p` is `0x...`, `%m`, `%ls` from UTF-32, `%Lf`
    narrowed from binary128). `vsnprintf` & co. take the `.so`'s `va_list`
    directly.
  - `pthread_bionic.cc`: bionic's pthread ABI over Windows threads and
    `WaitOnAddress` futexes on 32-bit words inside the bionic-sized objects,
    honouring bionic's static initializers (recursive/error-check type in
    bits 14-15 of the mutex's first word): attrs, create/join/detach/exit,
    mutexes (normal/recursive/error-check, timed), condition variables
    (timed, monotonic), rwlocks, once, keys with destructors at thread exit,
    `__cxa_thread_atexit_impl`.
  - Imports nothing defines: the loader binds a function import it cannot
    resolve to a stub that names the symbol and aborts **when called**, so a
    `.so` with unused missing imports still loads (`LoadOptions::
    stub_unresolved_functions`).

- **The runtime hook** (later steps): the host `OpenNativeLibrary`/`dlopen`
  path so `System.loadLibrary`/`System.load` find the app's `.so` through the
  ELF loader, and `JNI_OnLoad`/`RegisterNatives`/`Java_*` work through it.

## Steps and status

1. **Loader on a self-contained object.** `tests/native/trivial.c` is built by
   the NDK with `-nostdlib` (no `DT_NEEDED`, no TLS, no stack guard);
   `elf_load_test` loads it, checks the constructor ran, and calls its exports.
   Exercises mapping, `RELATIVE`/`RELR` and the init array, and the `fsbase`
   probe decides the `%fs` approach. **Done.**
2. The bionic shim: libc/libm/libdl/liblog symbols; a `.so` that uses
   `malloc`, `stdio`, `pthread`, `errno` (`tests/native/libc_test.c`).
   **Done.**
3. `%fs` for bionic TLS and the stack guard: a stack-protected /
   `thread_local` `.so`. **Next.**
4. The JNI path: `System.loadLibrary`, `JNI_OnLoad`, `RegisterNatives`,
   `Java_*`, and `JNIEnv` calls from the `.so` back into ART.
5. libc++_shared and a C++ exception; a test APK under `lib/x86_64`, run
   through gesher-app in CI.

## Building the test libraries

`.github/workflows/native-libs.yml` builds the `.so` files with a pinned
Android NDK (r27c, `27.2.12479018`) on the Linux runner and uploads them as the
`native-libs` artifact; the Windows `test` job loads them through
`elf_load_test` (and later through gesher-app).

## Status and next steps

_Last updated 2026-10-23 by the native-libraries track; branch
`wip/native-libs` off `phase3/base`. This session finished goal 1 (the bionic
shim) and stopped there by the coordinator's instruction; the next agent
starts with `%fs` (step 3 below)._

### Done and CI-verified

- **Step 1 — loader on a self-contained object.** `compat/elf/`, the
  `fsbase` probe and `elf_load_test`; the NDK builds `trivial.so` with
  `-nostdlib`; the loader maps it, applies RELATIVE/RELR, runs its constructor
  and calls its exports. First green: ci `39944128455`
  (`PASS  ELF loader: load a trivial .so and call it`, both runners).
- **Step 2 — the bionic shim.** `compat/bionic/` is the `gesher_bionic`
  target (`compat/bionic/bionic.cmake`, `include()`d from `CMakeLists.txt`),
  linked into `elf_load_test`, which calls `InstallBionicShim()` before
  loading. `native-libs.yml` builds `tests/native/libc_test.so` with the NDK
  (API 26, `DT_NEEDED` libc/libm/libdl/liblog, GNU hash only,
  `-fno-stack-protector`, no `thread_local`), and
  `tracks/40-native-libs.ps1` runs it: **ci `39948261535`, green on
  windows-2022 and windows-2025**:
  `PASS  bionic shim: a libc/pthread .so runs its checks` (and the step-1
  line). The `.so`'s own checks (each printed through the shim's `printf`)
  cover malloc/realloc/calloc/`posix_memalign(256)` + `free`, strings and
  64-bit `strtol`, `snprintf` (ints, `%ld`, widths, doubles, `%zu`/`%lld`/
  `%hhd`, `*` width/precision, `%p`, 18 arguments so some come from the
  stack, truncation), `sscanf`, `errno == ENOENT` from `open`, `fwrite` to
  `stdout`, `nanosleep`/`CLOCK_MONOTONIC`/`CLOCK_REALTIME`, libm, `qsort`/
  `bsearch` with a `.so` comparator, 4 threads on a mutex, a condition
  variable handshake, `pthread_once`, keys and their destructors at thread
  exit, a recursive mutex (and `trylock` from another thread gives `EBUSY`),
  `pthread_cond_timedwait` timing out, `dlsym(RTLD_DEFAULT)`/`dlerror`,
  `dl_iterate_phdr` finding the library's own `PT_LOAD`, `dladdr`, and
  `__android_log_print`.
  - The first attempt, ci `39947567513`, failed on a `-Werror` unused constant
    in `pthread_bionic.cc`, fixed in the next commit.

### Not done / stand-ins in what exists

- No roadmap box is ticked yet: the loader cannot yet run a normal NDK `.so`
  (they are built with `-fstack-protector-strong` by default, which reads
  `%fs:0x28`), and the bionic box also names libandroid, which the shim does
  not have.
- Shim gaps (none needed by the test): `opendir`/`readdir` (bionic's
  `struct dirent` differs), sockets, `select`/`poll`, signals, `fork`/`exec`,
  locale and wide-char functions, `getline`, `scanf`/`fscanf` from a `FILE*`,
  `%Lf` in `scanf`, `strtold`, `localtime`/`strftime`, `getauxval`.
  Missing **function** imports load as stubs that print the name and abort
  when called; missing **data** imports still fail the load.
- Threads ART creates (not through the shim's `pthread_create`) do not run
  bionic key destructors or `thread_local` destructors when they exit.
- `__system_property_get` returns "" for everything; `__android_log_*` writes
  `P/tag: text` to stderr.

### Next: step 3 — `%fs` for bionic TLS (the track's main open problem)

The `fsbase` probe (an INFO line in every run) still shows that `WRFSBASE`
executes but the base is lost on kernel entry, so ADR 0014's decision needs
replacing; update the ADR with the outcome. What a successor should know:

- **Where `%fs` shows up in NDK code.** `native-libs.yml` now prints, per
  `.so`, how many instructions use `%fs:` (`llvm-objdump -d | grep -c '%fs:'`);
  `trivial.so` and `libc_test.so` should report 0. In NDK output:
  - the stack guard, in every `-fstack-protector-strong` function:
    `mov %fs:0x28,%rax` (`64 48 8b 04 25 28 00 00 00`) in the prologue, and a
    `mov`/`cmp`/`sub`/`xor` with `%fs:0x28` in the epilogue: absolute
    `[disp32]` addressing (ModRM mod=00 rm=100, SIB 0x25);
  - `thread_local`: for **API < 29** clang uses *emulated* TLS
    (`__emutls_get_address`, linked into the `.so` from compiler-rt and built
    on `pthread_key_*`/`pthread_once`/`malloc`, all in the shim), so no `%fs`;
    for **API 29 and up**, ELF TLS in a shared object is general-dynamic
    (`__tls_get_addr`, in the shim) unless built with
    `-ftls-model=initial-exec`, which gives `mov %fs:0,%rax` (absolute) or
    `mov %fs:(%rax),%eax` (register-based) forms.
- **A load-time rewrite that keeps instruction lengths** (to be proven in
  CI): Windows keeps `%gs` at the TEB, so an absolute `%fs:[d]` can become
  `%gs:[d']` by changing only the prefix byte `0x64` to `0x65` and the 4-byte
  displacement:
  - `%fs:0x28` to `%gs:0x30` (`NT_TIB.Self`: per-thread, non-zero, constant
    for the thread's life; any stable value works as a stack cookie, since the
    prologue and epilogue read the same slot);
  - `%fs:0` (the thread pointer) to `%gs:[0x1480 + 8*i]`, a `TlsAlloc` slot
    (`TEB.TlsSlots[i]`, i < 64) holding the thread's variant-II block pointer,
    set by `SetupThreadTls` (so it must run on a thread before IE code does);
  - register-based forms (`%fs:(%reg)`) cannot be rewritten in place; trap
    them (e.g. `int3` and a vectored handler that emulates the `mov`) or reject
    the `.so` with a clear message; they only appear with initial-exec TLS.
  - Match only complete encodings (prefix 0x64, REX.W, a read opcode with a
    ModRM, SIB 0x25, disp32 0x28 or 0), and patch before `ProtectSegments`
    makes `.text` read-only. A linear-sweep decoder is more robust than byte
    matching if false positives (the pattern inside other instructions'
    immediates) turn up.
- `TlsGetAddr`/`BuildArena` (`compat/elf/tls.cc`) build a thread's block
  **once**; a `PT_TLS` module loaded after that gets a `tp_offset` outside the
  block. Give `__tls_get_addr` a per-thread, per-module fallback (allocated on
  first use) before relying on API 29+ libraries.
- The CI check for step 3: a `.so` built with the default stack protector and
  an emulated-TLS `thread_local` (API 26), and a second with ELF TLS
  (API 29), each used from several shim threads.

### After that (not started)

- **Step 4 — JNI path.** Hook the host `OpenNativeLibrary`/`dlopen`
  (`art/libnativeloader/native_loader.cpp`'s `!ART_TARGET_ANDROID` branch,
  which calls plain `dlopen`/`dlsym`; on Windows that is gesher-base's
  `dlopen` in `compat/posix/src/dlfcn.cc`, which knows builtin libraries and
  PE DLLs) so `System.loadLibrary` routes an ELF `.so` through the loader,
  and `JNI_OnLoad`/`RegisterNatives`/`Java_*` resolve through `elf::Lookup`.
  JNI is System V on both sides already (JNICALL is `sysv_abi`, GESHER_JNI
  wraps the JNIEnv/JavaVM tables for System V callers;
  [0006](decisions/0006-managed-abi.md), `tools/jni-sysv-patch.py`), so a
  `.so`'s `Java_*` functions and its calls through `JNIEnv` should need no
  extra bridging; verify. Any edit in `art/libnativeloader` patches must be
  listed in the final report.
- **Step 5 — libc++_shared and a C++ exception; a test APK under
  `lib/x86_64`, run through gesher-app.** libc++_shared's unwinder finds
  `.eh_frame_hdr` through `dl_iterate_phdr`, which the shim now provides
  (System V, over `elf::ForEachLoaded`, verified by `libc_test`);
  libc++_shared is stack-protected, so it needs step 3 first.

### Open problems and decisions

- `%fs`: above.
- **Static-only TLS**: every load-time module gets a static (initial-exec)
  offset; see the `TlsGetAddr` note above.
- The variadic bridge saves all six argument GPRs and eight XMMs regardless
  of `%al`, and assumes a `va_list` the `.so` passes is bionic's (a pointer to
  the 24-byte struct); both hold for NDK clang output.

### Dead ends ruled out

- **`WRFSBASE` per thread** (ADR 0014's first decision): the base does not
  survive kernel entry on windows-2022/2025 (the probe, every run).
- **Forwarding bionic pthread to winpthreads**: impossible; bionic's
  `pthread_mutex_t` is 40 bytes with a different layout than winpthreads', and
  the `.so` allocates the bionic-sized object. The shim implements the ABI
  directly instead.
- **`va_start` in a `sysv_abi` function** (to forward `printf`): not allowed
  on Windows targets; the assembly trampolines in `stdio_bionic.cc` replace
  it.
- **File-backed fixed segment mappings** for `PT_LOAD`: avoided; the loader
  reserves one anonymous range and copies segment contents, since file-backed
  `MAP_FIXED` over a reservation needs placeholder mappings gesher-base does not
  have yet.

### How to work on this

- Loader/shim logic can be syntax-checked locally without AOSP or WSL with the
  WinLibs UCRT `g++` already on the machine:
  `g++ -std=gnu++20 -fsyntax-only -Wall -Wextra -Werror -mfsgsbase -DGESHER
  -D_FILE_OFFSET_BITS=64 -D_WIN32_WINNT=0x0A00 -I compat/elf/include
  -I compat/elf -I compat/bionic/include -I compat/bionic
  -I compat/posix/include <file.cc>`.
  It is not the CI toolchain (llvm-mingw clang) but catches most `-Werror`
  issues cheaply before a full CI build; clang still finds a few more (e.g.
  `-Wunused-const-variable`).
- Poll CI gently (`gh run watch -i 90`, or the unauthenticated
  `https://api.github.com/repos/gesher-runtime/gesher/actions/runs/<id>`):
  several agents share the token and GitHub's secondary rate limit has
  blocked `gh run view` for minutes at a time.
- Full verification is CI only: `gh workflow run ci --ref wip/native-libs`
  (the reusable `native-libs.yml` runs because `ci` calls it; it cannot be
  dispatched on its own until it reaches the default branch).
