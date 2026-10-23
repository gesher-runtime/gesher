# Native x86-64 libraries (Phase 6)

Many Android apps ship native code as ELF `.so` files built by the NDK for
Linux, under `lib/x86_64` in the APK. Windows cannot load them (`LoadLibrary`
wants PE), so Gesher loads them itself: an **ELF loader** maps the shared
object and links it against a **bionic shim** (libc/libm/libdl/liblog and the
common NDK libraries) built on gesher-base, so `System.loadLibrary` and JNI
work both ways.

This is the native-libraries track; its decisions are
[0014](decisions/0014-native-libs-fs-base.md) (%fs/TLS) and
[0015](decisions/0015-native-libs-jni-bridge.md) (JNI). It builds in steps, each verified in CI
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

- **TLS and `%fs`** ([0014](decisions/0014-native-libs-fs-base.md)). Windows
  keeps no user `%fs` base across kernel entry (the `fsbase` probe), so the
  loader **rewrites the object's `%fs` accesses at load time**
  (`compat/elf/fs_rewrite.cc`, with the x86-64 length decoder
  `compat/elf/x86_decode.cc`): it decodes the code over the FDE ranges of
  `.eh_frame_hdr` and turns `%fs:0x28` (the stack guard) into `%gs:0x30`
  (`NT_TIB.Self`) and `%fs:0` (the thread pointer) into a `TlsAlloc` slot of
  the TEB, keeping instruction lengths; register-based `%fs:(%reg)` accesses
  (initial-exec TLS) are replayed in out-of-line stubs; anything else becomes
  an `int3` with a diagnostic. Each thread that runs loaded code gets a bionic
  variant-II block (`compat/elf/tls.cc`) with 1 MiB of static TLS reserved
  below the thread pointer, so a TLS library loaded later still gets a static
  offset and is copied into every live thread's block.

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

  - `locale_bionic.cc`: what libc++ needs of `<locale.h>`, `<wchar.h>` and
    `<wctype.h>`: `newlocale`/`uselocale` (C and UTF-8 locales, UTF-8 by
    default), `MB_CUR_MAX`, UTF-8 `mbrtowc`/`wcrtomb`/`mbsnrtowcs`/... on
    32-bit `wchar_t` and bionic's 8-byte `mbstate_t`, the `*_l`
    classification functions, wide string parsing, `strtold` (binary128).
    `syscall` (in `shim.cc`) does `futex` wait/wake (libc++abi's guards,
    `std::atomic::wait`) and `gettid`.

- **The JNI bridge** ([0015](decisions/0015-native-libs-jni-bridge.md),
  `compat/bionic/jni_bridge.cc`, linked into dalvikvm). ART's host
  `OpenNativeLibrary` calls plain `dlopen`; gesher-base's `dlopen` hands a
  file with the ELF magic to the loader registered through
  `<gesher/elf_dlopen.h>`, which is this bridge. `dlsym` on such a handle
  gives ART a Microsoft-ABI stub for `JNI_OnLoad`/`JNI_OnUnload`, and for
  `Java_*` (and functions passed to `RegisterNatives`) a System V entry stub
  that sets the thread up (`SetupThreadTls`) and swaps ART's `JNIEnv*` for a
  per-thread proxy. The `.so` only sees proxy `JNIEnv`/`JavaVM` objects whose
  System V tables forward to ART's Microsoft-ABI ones; variadic and `va_list`
  calls are turned into `jvalue` arrays by the method's parameter types.
  DT_NEEDED libraries are looked up next to the library (an APK's
  `libc++_shared.so`).

- **gesher-app** copies the APK's `lib/x86_64/*.so` into
  `ApplicationInfo.nativeLibraryDir` before launching
  (`app/java/com/gesher/app/NativeLibs.java`), which the app's class loader
  searches for `System.loadLibrary`.

## Steps and status

1. **Loader on a self-contained object.** `tests/native/trivial.c` is built by
   the NDK with `-nostdlib` (no `DT_NEEDED`, no TLS, no stack guard);
   `elf_load_test` loads it, checks the constructor ran, and calls its exports.
   Exercises mapping, `RELATIVE`/`RELR` and the init array, and the `fsbase`
   probe decides the `%fs` approach. **Done.**
2. The bionic shim: libc/libm/libdl/liblog symbols; a `.so` that uses
   `malloc`, `stdio`, `pthread`, `errno` (`tests/native/libc_test.c`).
   **Done.**
3. `%fs` for bionic TLS and the stack guard: stack-protected `.so` files
   with emulated (API 26) and ELF (API 29) thread-locals, threads, sleeps, and
   a TLS library loaded while a thread runs (`tests/native/fs_test.c`,
   `fs_late.c`). **Done.**
4. The JNI path: `System.loadLibrary`, `JNI_OnLoad`, `RegisterNatives`,
   `Java_*`, and `JNIEnv` calls from the `.so` back into ART
   (`tests/native/jni_test.c`, `tests/native/java/gesher/JniTest.java`).
   **Done.**
5. libc++_shared and a C++ exception (`tests/native/libcxx_test.cpp`); a test
   APK under `lib/x86_64`, run through gesher-app in CI
   (`tests/apps/nativeapp`). **Done.**

## Building the test libraries

`.github/workflows/native-libs.yml` builds the `.so` files with a pinned
Android NDK (r27c, `27.2.12479018`) on the Linux runner, the JNI test's dex
(javac + d8 9.5.26), and `nativeapp.apk` (`tests/apps/nativeapp/build.sh`:
the NDK, aapt2/zipalign from build-tools r36.1, the SDK's `android.jar` from
`platform-36_r02`, d8), and uploads them as the `native-libs` artifact; the
Windows `test` job runs them through `elf_load_test`, dalvikvm and
gesher-app.

## Status and next steps

_Last updated 2026-10-23 by the native-libraries track; branch
`wip/native-libs` off `phase3/base`. Steps 1-5 are done and CI-verified; see
"After that" for what is left._

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
- **Step 3 — `%fs` by load-time rewrite** (ADR 0014's revised decision).
  `compat/elf/x86_decode.cc` (length decoder), `compat/elf/fs_rewrite.cc`
  (rewrite, stubs, vectored handler for `int3` sites and diagnostics),
  `compat/elf/tls.cc` (TLS slot, per-thread blocks with a static surplus,
  late modules, dynamic-only fallback, blocks freed at thread exit through
  FLS). `WRFSBASE` is gone from the loader. `native-libs.yml` builds
  `fs_test26.so` (API 26, emulated TLS), `fs_test29.so` (API 29, ELF TLS:
  general-dynamic, local-dynamic and initial-exec variables) and `fs_late.so`
  (API 29, initial-exec + general-dynamic), all `-fstack-protector-strong`;
  the build log counts 14, 25 and 4 instructions using `%fs` in them,
  including `movq %fs:0x28`, `movq/addq %fs:0x0`, `movl %fs:(%rcx)`,
  `cmpl $0x7, %fs:(%r15)`, `cmpb $0x0, %fs:(%rcx)` and
  `movl $0x63, %fs:(%rax)`. **ci `39955700023`, green on windows-2022 and
  windows-2025**:
  `PASS  native libs: stack-protected .so with emulated TLS (API 26) under the %fs rewrite`,
  `PASS  native libs: stack-protected .so with ELF TLS (API 29), threads and a late TLS library`
  (and the step-1/2 lines). `elf_load_test` requires that the library's
  expected kinds of rewrite happened (stack guard; thread pointer;
  out-of-line for API 29) and that nothing was left un-rewritable; the
  library checks the guard and thread pointer are non-zero, per-thread and
  stable across sleeps (a stack-protected function sleeps between prologue
  and epilogue), its thread-locals' initial values in new threads and their
  per-thread values across sleeps in 4 threads, `&tls` inside the thread's
  block, and, for API 29, `dlopen("fs_late.so")` while a thread is running:
  that thread sees the late library's initial TLS values and keeps its own.
  The rewriter and stubs were also exercised locally with a hand-built ELF
  image (every form above plus indexed, store and flag-preserving cases, the
  `int3` and `jmp` entries, and the abort diagnostic), and the decoder was
  compared with `objdump` (identical boundaries over 7.2 M instructions of a
  GCC binary; an AVX-512 OpenBLAS differed only on `fwait`-merged x87 mnemonics
  and data in `.text`).
- **Step 4 — the JNI path** ([0015](decisions/0015-native-libs-jni-bridge.md)).
  `<gesher/elf_dlopen.h>` and the hook in gesher-base's `dlfcn.cc`;
  `compat/bionic/jni_bridge.cc` (+ `jni.cmake`) in dalvikvm. dalvikvm runs
  `gesher.JniTest` (dex built in `native-libs.yml`), whose `System.load` of
  `jni_test.so` (API 29, stack protector, an initial-exec thread-local) goes
  through ART's unpatched host libnativeloader path. **ci `39981957010`, green
  on windows-2022 and windows-2025**:
  `PASS  native libs: System.load of an NDK .so; JNI_OnLoad, RegisterNatives, Java_* and JNIEnv calls into ART`.
  The check requires `JNI_OnLoad ran: ok` and `JniTest all passed`, i.e.:
  `JNI_OnLoad` (GetEnv, FindClass, NewGlobalRef, RegisterNatives of a static
  and an instance native), natives by name with strings both ways, 15
  arguments of mixed types (two integer and one double on the stack),
  `ThrowNew` caught in Java; from native code: `FindClass`, static/virtual/
  non-virtual calls in variadic and `jvalue` forms, 10 mixed variadic
  arguments (some on the stack), fields, `NewObject`, arrays, a Java
  exception seen and cleared, `GetJavaVM`/`GetEnv` giving the same proxy,
  local frames; a variadic call with a `jmethodID` from
  `FromReflectedMethod` (parameter types by reflection); a pthread of the
  library's own that attaches, calls Java and detaches; and initial-exec TLS
  on the main thread and on another thread ART created (each starts at its
  initial value and keeps its own).
- **Step 5 — libc++_shared and C++ exceptions; the test APK.**
  `locale_bionic.cc` and `syscall` (futex) in the shim. `libcxx_test.so`
  (API 29, C++, `DT_NEEDED libc++_shared.so`, the NDK's copy beside it), run
  by `elf_load_test`: **ci `39981957010`, both runners**:
  `PASS  native libs: libc++_shared through the loader; C++ exceptions thrown and caught in the .so`
  (requires `libcxx_test: all passed`): `std::runtime_error` thrown and
  caught, `std::out_of_range` thrown by libc++ itself (`vector::at`) and
  caught in the library, an exception through 6 frames caught as a base class
  with 6 destructors run during unwinding, `catch (...)` and rethrow,
  `exception_ptr`, `std::stoi`'s `invalid_argument`, `dynamic_cast`/`typeid`/
  `bad_cast`, 4 `std::thread`s throwing and catching 50 times each,
  `std::to_string`. The unwinder is libc++_shared's own (LLVM libunwind),
  finding `.eh_frame_hdr` through the shim's `dl_iterate_phdr`; it needed no
  loader change. `nativeapp.apk` (`tests/apps/nativeapp`:
  `lib/x86_64/libnativeapp.so`, C++ against `libc++_shared.so`, both in the
  APK, stored) through gesher-app: **ci `39981957010`, both runners**:
  `PASS  native libs: gesher-app runs nativeapp.apk; lib/x86_64 loaded from the APK, native result checked in onResume`
  (requires `extracted 2 native libraries (x86_64)`,
  `NativeActivity onResume, native=Hello from C++, caught std::out_of_range, callBack(12)=145`,
  `gesher-app: com.gesher.nativeapp.NativeActivity onResume` and
  `gesher-app: done`): gesher-app extracts `lib/x86_64`, the Activity's
  `onCreate` calls `System.loadLibrary("nativeapp")` through the app's
  `PathClassLoader`, the library loads `libc++_shared.so` from the same
  directory, catches a libc++ exception, and calls back into the Activity
  through C++'s `JNIEnv::CallIntMethod` (which uses the `va_list` form).
- Roadmap: the "ELF loader" box (ci `39955700023`) and the
  "`System.loadLibrary` of an app's NDK library from its APK's `lib/x86_64`,
  with JNI calls both ways" box (ci `39981957010`) are ticked. The bionic box
  is not: it names libandroid, which the shim does not have.

### Not done / stand-ins in what exists

- **Thread setup for initial-exec TLS.** The stack guard (`%gs:0x30`) and
  `__tls_get_addr` work on any thread; `%fs:0`-derived (initial-exec)
  accesses need the thread's block, made by `SetupThreadTls()`: the shim's
  `pthread_create` threads, the JNI bridge's native entry stub, its
  `JNI_OnLoad` bridge and `AttachCurrentThread`, and `elf_load_test`'s main
  thread call it. Loaded code reached any other way (a `.so` function pointer
  that Windows code calls directly) would fault with a named diagnostic from
  the loader's vectored handler.
- **JNI** ([0015](decisions/0015-native-libs-jni-bridge.md)): `@CriticalNative`
  natives in an app library pass the entry stub untouched, but that is not
  tested. JVMTI environments from `GetEnv` are not wrapped. `dlsym` results
  other than `JNI_OnLoad`/`JNI_OnUnload`/`Java_*` are raw System V
  addresses. No linker namespaces: all app libraries share one global scope.
  `dlclose` (class loader unloading) runs fini functions but never unmaps.
- **libc++ imports the shim still lacks** (they load as stubs that abort if
  called; the tests do not reach them): `std::filesystem`'s (`opendir`,
  `readdir`, `fdopendir`, `closedir`, `realpath`, `getcwd`, `chdir`,
  `openat`, `unlinkat`, `symlink`, `link`, `readlink`, `truncate`,
  `ftruncate`, `fchmod`, `fchmodat`, `utimensat`, `pathconf`, `statvfs`,
  `sendfile`), wide stdio (`getwc`, `ungetwc`, `fputwc`, `swprintf`), and
  `ioctl`. The loader prints one "not implemented (stubbed)" line per such
  import when libc++_shared.so loads. Locales other than C/UTF-8 do not exist
  (collation and `strftime_l` are the C locale's).
- **The test APK** is built against the public SDK's `android.jar`
  (platform 36 r2), not Gesher's framework classes; its native code is API 26
  (emulated TLS) and its `libc++_shared.so` is the NDK's.
- The stack cookie is the TEB address: correct, not secret.
- Un-rewritable `%fs` forms (other TCB displacements, RIP-relative, VEX with a
  register-based `%fs` operand, partial writes of the base register) load as
  `int3` sites that abort with a message if reached; none occur in the test
  libraries.
- Shim gaps (none needed by the tests): `opendir`/`readdir` (bionic's
  `struct dirent` differs), sockets, `select`/`poll`, signals, `fork`/`exec`,
  wide-char stdio, `getline`, `scanf`/`fscanf` from a `FILE*`,
  `%Lf` in `scanf`, `strtold`, `localtime`/`strftime`, `getauxval`, and no
  libandroid. Missing **function** imports load as stubs that print the name
  and abort when called; missing **data** imports still fail the load.
- Threads ART creates (not through the shim's `pthread_create`) do not run
  bionic key destructors or `thread_local` destructors when they exit.
- `__system_property_get` returns "" for everything; `__android_log_*` writes
  `P/tag: text` to stderr.
- Closing a library does not unmap it or release its TLS offset.

### After that (not started)

- **libandroid** (`ANativeWindow`, `AAssetManager`, `ALooper`, ...), for the
  roadmap's bionic box; `AAssetManager` over the framework's AssetManager is
  what real apps need most often.
- The libc++ imports listed above (`std::filesystem`, wide stdio) and the
  other shim gaps, as real apps need them.
- Real-world APKs through gesher-app: the NDK samples (hello-jni,
  hello-libs), and C++ code registering natives from tables.
- A correction to this file's earlier plan: JNI was not "System V on both
  sides already". ART's `JNIEnv`/`JavaVM` tables are Microsoft ABI
  (`GESHER_JNI` only wraps natives the framework registers); the bridge's
  proxies are what make a `.so`'s `JNIEnv` calls work
  ([0015](decisions/0015-native-libs-jni-bridge.md)).

### Open problems and decisions

- `%fs`: solved by load-time rewriting (ADR 0014, revised). Code generated
  at run time inside the app (its own JIT) that uses `%fs` is not covered.
- **Static TLS reserve**: 1 MiB per thread of address space (committed on
  demand); a module beyond it is dynamic-only (general-dynamic works,
  initial-exec relocations against it fail the load with a message).
- The variadic bridge saves all six argument GPRs and eight XMMs regardless
  of `%al`, and assumes a `va_list` the `.so` passes is bionic's (a pointer to
  the 24-byte struct); both hold for NDK clang output.

### Dead ends ruled out

- **`WRFSBASE` per thread** (ADR 0014's first decision): the base does not
  survive kernel entry on windows-2022/2025 (the probe, every run); replaced
  by the load-time rewrite.
- **Matching `%fs` byte patterns without decoding**: the plan's first idea;
  replaced by decoding from FDE starts, which also finds the register-based
  and immediate forms the compiler emits (`cmpl $7, %fs:(%r15)`,
  `addq %fs:0, %r15`) and never mistakes data for a prefix.
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
