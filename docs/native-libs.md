# Native x86-64 libraries (Phase 6)

Many Android apps ship native code as ELF `.so` files built by the NDK for
Linux, under `lib/x86_64` in the APK. Windows cannot load them (`LoadLibrary`
wants PE), so Gesher loads them itself: an **ELF loader** maps the shared
object and links it against a **bionic shim** (libc/libm/libdl/liblog and the
common NDK libraries) built on gesher-base, so `System.loadLibrary` and JNI
work both ways.

This is the native-libraries track; its decisions are
[0014](decisions/0014-native-libs-fs-base.md) (%fs/TLS),
[0017](decisions/0017-native-libs-jni-bridge.md) (JNI),
[0020](decisions/0020-native-libs-libandroid.md) (libandroid, the rest of
libc, signals) and [0023](decisions/0023-native-libs-ndk-graphics.md)
(ANativeWindow, EGL/GLES over ANGLE). It builds in steps, each verified in CI
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
  - `posix_bionic.cc`: directories with bionic's 280-byte `struct dirent`
    (`opendir`, `fdopendir` owning its fd, `readdir`/`readdir_r`, `dirfd`,
    `scandir`/`alphasort`); the `*at()` calls (`openat`, `fstatat`,
    `unlinkat`, `mkdirat`, `renameat`, `faccessat`, `fchmodat`, `utimensat`,
    `readlinkat`, `linkat`, `symlinkat`) by resolving the directory fd's
    path; `unlink`/`rmdir`/`remove` with a POSIX delete disposition (the name
    goes at once even while open, as Linux's); `getcwd`, `chdir`, `realpath`,
    `truncate`/`ftruncate`, `fchmod`, `statvfs` (bionic's 24 reserved bytes),
    `sendfile`, `pathconf` and **`sysconf` by bionic's own `_SC_*`/`_PC_*`
    numbers** (the shim used to pass them to gesher-base, which uses glibc's);
    `fcntl` (record locks are always granted: not modelled), `ioctl`,
    `getauxval`, `uname`, ids, rlimits; `environ`/`__progname` data; the
    FORTIFY `__read_chk`/`__write_chk`/`__pread_chk`/`__getcwd_chk`/... forms;
    `stpcpy`, `memrchr`, `strsep`, `strcasestr`, `basename`, `dirname`.
  - `socket_bionic.cc`: BSD sockets over gesher-base's Winsock layer (same
    Linux structures; `SO_RCVTIMEO`/`SO_SNDTIMEO` convert bionic's 64-bit
    `timeval`), `getaddrinfo`/`getnameinfo` with bionic's BSD `addrinfo`
    field order and `AI_*`/`EAI_*`/`NI_*` values, `inet_*`, `poll`/`ppoll`,
    `select`/`pselect` over `poll` with bionic's 1024-bit `fd_set` and
    `__FD_*_chk`, `epoll_*`, `eventfd`, `pipe`/`pipe2`, `readv`/`writev`;
    `in6addr_any`/`in6addr_loopback` data.
  - `signal_bionic.cc`: signals in user space, separate from gesher-base's and
    ART's (libsigchain): `sigaction` (bionic's LP64 layout, System V
    handlers, `SA_SIGINFO` with a Linux `siginfo_t`, `SA_RESETHAND`,
    `SA_NODEFER`), `signal`, per-thread masks, `raise`/`kill`/`sigqueue`/
    `pthread_kill`, `sigpending`, `sigwait*`, `sigsuspend`, `pause`,
    `strsignal`, and `abort()` running a `SIGABRT` handler first. See "Not
    done" for what Windows does not allow.
  - `time_bionic.cc`: bionic's 56-byte `struct tm` (`tm_gmtoff`, `tm_zone`):
    `localtime(_r)`, `gmtime(_r)`, `mktime`, `timegm`, `tzset` with the
    `tzname`/`timezone`/`daylight` data, `asctime`/`ctime`, and a C-locale
    `strftime`/`strftime_l` with every conversion and flag bionic's has (the
    CRT's aborts on unknown ones).
  - In `stdio_bionic.cc` too: `fscanf`/`scanf`/`vfscanf` (and `%Lf` into
    binary128 for the whole scanf family), `getline`/`getdelim`, wide stdio
    (`fgetwc`/`getwc`, `ungetwc`, `fputwc`/`putwc`, `fputws`, `fwide`,
    `swprintf`/`vswprintf`), `freopen`, `fgetpos`/`fsetpos`, the FILE FORTIFY
    forms; `fopen` modes are always binary (`e` and `x` understood), and
    `fopen` opens through gesher-base's `open`, which shares delete access,
    so an open file can be unlinked or renamed as on Linux.
- **libandroid** (`compat/bionic/libandroid.cc`, target `gesher_libandroid`
  in `compat/bionic/jni.cmake`, linked into dalvikvm next to the framework's
  natives): the NDK's non-graphics `libandroid.so` APIs as System V entries
  that its static initializer adds to the shim's table, so an app's
  `DT_NEEDED libandroid.so` is satisfied symbol by symbol like `libc.so`:
  - `AAssetManager_fromJava` → the Java `AssetManager`'s native
    `AssetManager2` (through `android_util_AssetManager.cpp`'s
    `NdkAssetManagerForJavaObject`; the `.so`'s `JNIEnv` is the bridge's
    proxy, so the bridge's `RealJniEnv` gives the runtime's), `AAssetManager_
    open`/`openDir`, all `AAsset_*` (64-bit `off_t`), `AAssetDir_*` — the
    APK's assets as on Android;
  - `AConfiguration_*` on a `ResTable_config`, `fromAssetManager` reading the
    `AssetManager2`'s configuration;
  - `ALooper_*`: an `ALooper*` is libutils' `Looper*`, as on Android, so the
    main thread's `ALooper_forThread()` is the Java main Looper;
    `ALooper_addFd`'s System V callback is wrapped in a `LooperCallback` that
    sets the thread up for loaded code; `ALooper_pollOnce`/`pollAll`/`wake`/
    `removeFd`/`acquire`/`release`;
  - `ASensorManager_*`/`ASensorEventQueue_*`/`ASensor_*`: a device with no
    sensors (an empty list, queues that never deliver);
  - `ATrace_*`: no-ops, `isEnabled` false;
  - `AStorageManager_*`: mounting OBBs is not supported (the callback gets
    `AOBB_STATE_ERROR_INTERNAL` from another thread, as the platform's comes
    from a binder thread); `AObbScanner_getObbInfo`/`AObbInfo_*` read the
    file with androidfw's `ObbFile`;
  - `ASharedMemory_create`/`getSize`/`setProt` over gesher-base's
    `memfd_create`;
  - `ANativeWindow_fromSurface`/`toSurface` over libandroid_runtime's, with
    the runtime's `JNIEnv`; the other `ANativeWindow_*` are in
    `libandroid_window.cc` ("Graphics" below);
  - `ANativeActivity_*`: stand-ins (no NativeActivity natives yet; below).
- **Graphics** ([0023](decisions/0023-native-libs-ndk-graphics.md),
  `compat/bionic/ndk_graphics.cmake`: the OBJECT library
  `gesher_ndk_graphics`, linked into dalvikvm and `gl_window_test`):
  - `libandroid_window.cc`: `ANativeWindow_acquire`/`release`/`getWidth`/
    `getHeight`/`getFormat`/`setBuffersGeometry`/`lock`/`unlockAndPost`/
    `setBuffersTransform`/`setBuffersDataSpace`/`getBuffersDataSpace`/
    `getBuffersDefaultDataSpace`/`setFrameRate`/
    `setFrameRateWithChangeStrategy`/`tryAllocateBuffers` as System V thunks
    over dalvikvm's Microsoft-ABI functions
    (`compat/hwui/window/gesher_native_window.h`: hostgraphics' and the
    graphics track's), which work on any hostgraphics `Surface` — a
    `Win32Surface` for a Gesher window. `ANativeWindow_clearFrameRate` is
    inline in the NDK's header.
  - `egl_gles.cc` + `gen_gl_thunks.py`: every function of AOSP's
    `frameworks/native/opengl/include` (EGL 1.5 and extensions, GLES 1.x,
    2.0, 3.0-3.2 and extensions: 1,182, 14 of them handwritten) as a System V
    thunk to ANGLE's `libEGL.dll`/`libGLESv2.dll` (loaded from the
    executable's directory on first use; a function ANGLE lacks aborts naming
    itself), so an app's `libEGL.so`/`libGLESv1_CM.so`/`libGLESv2.so`/
    `libGLESv3.so` imports and `eglGetProcAddress` all resolve.
    `EGL_DEFAULT_DISPLAY` is ANGLE's Direct3D 11 display on the hardware
    adapter, else WARP (`GESHER_EGL_DEVICE=warp` forces WARP; the choice is
    printed as `gesher EGL: default display: ...`). `eglCreateWindowSurface`
    (and the platform forms) takes the `ANativeWindow*` and creates ANGLE's
    window surface on its HWND (`gesher_window_hwnd`), holding a reference
    to the window until `eglDestroySurface`. Android-only config attributes
    are dropped/answered, `EGL_NATIVE_VISUAL_ID` is a HAL pixel format,
    callbacks get Microsoft-ABI trampolines.
  - **Presenting when GL owns the HWND**: the Gesher window paints its CPU
    back buffer on `WM_PAINT`; while an EGL window surface is on it,
    `egl_gles.cc` subclasses the HWND so `WM_PAINT` only validates (the
    swap chain presents), and restores it with the last surface. Nothing in
    `compat/hwui/window` changed.

- **Unresolved data imports** no longer fail a load: the loader binds each to
  its own slot in a reserved, inaccessible region and prints "data '...' is
  not provided"; touching it is caught by a vectored handler that names the
  symbol and aborts (like the stubs for missing functions).

- **The JNI bridge** ([0017](decisions/0017-native-libs-jni-bridge.md),
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
6. libandroid's non-graphics APIs and the shim's gaps (directories, the
   `*at()` calls, sockets, poll/select, signals, time, `fscanf`, wide stdio,
   the libc++ imports `std::filesystem` needs); the test APK's native code
   uses them through gesher-app. **Done.**
7. libandroid's graphics half and EGL/GLES: an NDK library draws into a
   Gesher window with `ANativeWindow_lock` and with GLES 3/2 on an EGL window
   surface through ANGLE, read back in CI (`tests/apps/glapp`,
   `tests/native/gl_window_test.cc`). **Done** (`ANativeActivity` is not:
   see below).

## Building the test libraries

`.github/workflows/native-libs.yml` builds the `.so` files with a pinned
Android NDK (r27c, `27.2.12479018`) on the Linux runner, the JNI test's dex
(javac + d8 9.5.26), and `nativeapp.apk` (`tests/apps/nativeapp/build.sh`:
the NDK, aapt2/zipalign from build-tools r36.1, the SDK's `android.jar` from
`platform-36_r02`, d8), `glapp.apk` and `libglapp.so`
(`tests/apps/glapp/build.sh`, C against libandroid/libEGL/libGLESv3), and uploads them as the `native-libs` artifact; the
Windows `test` job runs them through `elf_load_test`, dalvikvm and
gesher-app.

## Status and next steps

_Last updated 2026-10-23 by the native-libraries track; branch
`wip/native-libs` (on the integration branch with the graphics track's
window and ANGLE). Steps 1-7 are done and CI-verified (latest: ci
`40013029994`, green on windows-2022 and windows-2025); `ANativeActivity`
waits for Activity windows; see "After that" for what is left._

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
- **Step 4 — the JNI path** ([0017](decisions/0017-native-libs-jni-bridge.md)).
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
- **Step 6 — libandroid and the shim's gaps.** `libandroid.cc`,
  `posix_bionic.cc`, `socket_bionic.cc`, `signal_bionic.cc`,
  `time_bionic.cc` and the stdio additions (see "Pieces"). `nativeapp.apk`
  now carries `assets/hello.txt` (and `assets/data/inner.txt`), its library
  links `-landroid -llog`, and the Activity's `onCreate` calls
  `platformChecks(getAssets(), dataDir)`; one gesher-app run gives both
  nativeapp lines. **ci `39987881014`, green on windows-2022 and
  windows-2025** (all earlier lines still pass):
  `PASS  native libs: gesher-app runs nativeapp.apk; libandroid assets/looper and libc sockets/readdir/std::filesystem/signals from the .so`.
  It requires
  `NativeActivity platform: asset=Hello from an asset, assets=hello.txt, config=ok, sensors=0, looper=ok, mainlooper=armed, socketpair=ok, tcp=ok, getaddrinfo=ok, readdir=a.txt/b.txt/sub, fscanf=ok, filesystem=ok, signals=ok, strftime=ok, sysconf=ok`
  and `NativeActivity main looper: fd callback ran`, i.e.: the asset's
  contents through `AAssetManager_fromJava`/`AAssetManager_open`
  (`AASSET_MODE_BUFFER`, `getBuffer`, `seek`/`read`/`getRemainingLength`),
  a missing asset giving `nullptr`, `AAssetManager_openDir("")` listing only
  the regular file; `AConfiguration_fromAssetManager` and a setter/getter;
  an empty sensor list; on the library's own thread `ALooper_prepare`, a pipe
  with a callback and an eventfd with ident 7 on `ALooper_addFd`, both seen by
  `ALooper_pollOnce` (fd, events, data), then `ALooper_wake` from a third
  thread ending a 10 s poll with `ALOOPER_POLL_WAKE`; a pipe added to the main
  thread's Looper (Java's), whose callback the Java message loop ran before the
  message `onResume` posted; `socketpair` with `poll`, `select` (fortified
  `FD_SET`), `send`/`recv`; TCP over loopback (`bind` to port 0,
  `getsockname`, `SO_RCVTIMEO` round trip, `connect`, `accept`); numeric
  `getaddrinfo` and `EAI_NONAME`; `readdir` names and `d_type`; `fscanf` and
  `getline`; `std::filesystem` (`create_directories`, `ofstream`/`ifstream`,
  `file_size`, `copy_file` (sendfile), `rename`, `recursive_directory_iterator`,
  `canonical`, `current_path`, `remove_all` of 5 entries while a file in it is
  still open); `SIGUSR1` with `SA_SIGINFO` through `raise`, a blocked
  `SIGUSR2` from `kill` pending and delivered by `sigprocmask`, `SIG_IGN`;
  `strftime` (`%Y-%m-%d %H:%M:%S %a %j %z %e|%k|%P`), `timegm`,
  `localtime_r`/`mktime`; `sysconf(_SC_PAGESIZE)` and
  `_SC_NPROCESSORS_ONLN`. With the new shim, `libc++_shared.so` loads with no
  "not implemented (stubbed)" import at all (it had 26).
  The first run of this check (ci `39987006747`) failed only on `remove_all`
  (`ENOTEMPTY`: Windows' delete-pending names), which led to the POSIX
  delete disposition and the share-delete `fopen`.
- **Step 7 — ANativeWindow and EGL/GLES** ([0023](decisions/0023-native-libs-ndk-graphics.md)).
  `libandroid_window.cc`, `egl_gles.cc`, `gen_gl_thunks.py`
  (`ndk_graphics.cmake`); `tests/apps/glapp` (`libglapp.so`, C, `DT_NEEDED`
  libandroid/libEGL/libGLESv3) and `tests/native/gl_window_test.cc`.
  **ci `40013029994` (commit a6b8891), green on windows-2022 and
  windows-2025**:
  `PASS  native libs: ANativeWindow_lock from an NDK .so into a Win32 window, read back with PrintWindow`,
  `PASS  native libs: EGL/GLES 3 and 2 from an NDK .so through ANGLE onto a window surface; glReadPixels`,
  `PASS  native libs: the GLES 3 and 2 frames presented in the window, read back with PrintWindow`,
  `PASS  native libs: the same EGL/GLES window surfaces on WARP`,
  `PASS  native libs: gesher-app runs glapp.apk; ANativeWindow_fromSurface and lock into a Surface, read back`,
  `PASS  native libs: gesher-app runs glapp.apk; EGL/GLES 3 and 2 on a Surface's window from the .so`
  (and every earlier line). `gl_window_test` makes 160x96 windows with
  `gesher_window_create`, shows them and loads `libglapp.so` through the
  loader: the library checks `getWidth`/`getHeight`/`getFormat`, sets RGBA_8888
  with `setBuffersGeometry`, locks (160x96, stride 160, format 1), writes four
  colored quadrants and posts; PrintWindow reads all 15,360 pixels back
  correct. Then, per window, EGL on `EGL_DEFAULT_DISPLAY` ("gesher EGL:
  default display: ANGLE, Direct3D 11 hardware device"; the runners'
  hardware adapter is the Basic Render Driver), `eglChooseConfig` with
  `EGL_RECORDABLE_ANDROID`, `EGL_NATIVE_VISUAL_ID` 1 into
  `setBuffersGeometry`, `eglCreateWindowSurface` on the `ANativeWindow*`, an
  ES 3 (then ES 2) context, `eglGetProcAddress("glClearColor")` called, a
  blue clear and a green triangle (GLSL ES 3.00 with a VAO, then GLSL ES
  1.00), `glReadPixels`: 1,920 green, 13,440 blue, 0 other, `eglSwapBuffers`;
  `PrintWindow(PW_RENDERFULLCONTENT)` then reads the presented frame back
  from the window (center green, corners blue, 1,920 green). The same again
  with `GESHER_EGL_DEVICE=warp` ("Direct3D 11 WARP device"). Through
  gesher-app, `glapp.apk`'s Activity makes windows with gesher-app's
  `NativeWindow` (reflection; Activities have no windows yet), passes their
  `android.view.Surface` to the library (`ANativeWindow_fromSurface`), reads
  the locked frame back with `NativeWindow.capture()` and gets ES 3 and ES 2
  frames checked by `glReadPixels`. ES 2 contexts report "OpenGL ES 3.0.0"
  (ANGLE gives the newest compatible version, as Android drivers may). The
  first runs found that Windows widens a captioned window's client area to
  about 120 pixels (a 96-pixel request gave a 120-pixel window while
  `ANativeWindow_getWidth` said 96: noted for the graphics track), and that
  on windows-2022 a shown window's read-back can precede its repaint (the
  test retries for up to 2 s); copying the screen under the window never
  sees it on the runners.
- Roadmap: the "ELF loader" box (ci `39955700023`), the
  "`System.loadLibrary` of an app's NDK library from its APK's `lib/x86_64`,
  with JNI calls both ways" box (ci `39981957010`) and the bionic box
  (libc, libm, libdl, liblog and libandroid's non-graphics APIs, ci
  `39987881014`; `libc_test.so` covers libm, libdl and liblog, the app covers
  libc and libandroid) are ticked, and the NDK graphics box (`ANativeWindow`
  and EGL/GLES from an app's `.so`, ci `40013029994`). `ANativeActivity`,
  `AChoreographer` and `AInputQueue` are not done.

### Not done / stand-ins in what exists

- **Thread setup for initial-exec TLS.** The stack guard (`%gs:0x30`) and
  `__tls_get_addr` work on any thread; `%fs:0`-derived (initial-exec)
  accesses need the thread's block, made by `SetupThreadTls()`: the shim's
  `pthread_create` threads, the JNI bridge's native entry stub, its
  `JNI_OnLoad` bridge and `AttachCurrentThread`, and `elf_load_test`'s main
  thread call it. Loaded code reached any other way (a `.so` function pointer
  that Windows code calls directly) would fault with a named diagnostic from
  the loader's vectored handler.
- **JNI** ([0017](decisions/0017-native-libs-jni-bridge.md)): `@CriticalNative`
  natives in an app library pass the entry stub untouched, but that is not
  tested. JVMTI environments from `GetEnv` are not wrapped. `dlsym` results
  other than `JNI_OnLoad`/`JNI_OnUnload`/`Java_*` are raw System V
  addresses. No linker namespaces: all app libraries share one global scope.
  `dlclose` (class loader unloading) runs fini functions but never unmaps.
- **libc++**: every import of the NDK's `libc++_shared.so` now resolves.
  Locales other than C/UTF-8 do not exist (collation and `strftime_l` are the
  C locale's). Symbolic links: `symlink` fails with `EPERM` and `readlink`
  with `EINVAL` (gesher-base's), so `std::filesystem`'s symlink operations
  report errors.
- **Signals** (`signal_bionic.cc`) are the shim's own bookkeeping, since
  Windows has none: a signal for the calling thread runs its handler before
  `raise`/`kill`/`pthread_kill` returns; one for another thread (or blocked)
  is pending and runs at that thread's next delivery point (lowering its
  mask, `sigsuspend`/`pause`, and the shim's sleeps, `poll`, `select`,
  `epoll_wait`, `ALooper_pollOnce`), so it cannot interrupt a thread busy or
  blocked elsewhere. Not done: **synchronous signals from faults** (`SIGSEGV`,
  `SIGBUS`, `SIGFPE`, `SIGILL`, `SIGTRAP`) do not reach the `.so`'s handlers
  (a crash reporter's): Windows exceptions in loaded code go to ART's fault
  handling and the loader's diagnostics; the handler gets no machine context
  (`ucontext` is zeroed); `alarm`/`setitimer` timers; `sigaltstack` is only
  recorded; `kill` of other processes is `ESRCH`. A `SIG_DFL` that would
  terminate does (`abort()` for the core-dumping signals).
- **libandroid stand-ins**: sensors (none), tracing (off), OBB mounting
  (unsupported; scanning works). Not provided at all (they bind to the
  aborting stubs): `AChoreographer_*`, `AInputQueue_*`/`AInputEvent_*`,
  `ASurfaceControl_*`/`ASurfaceTexture_*`, `AHardwareBuffer_*`,
  `AThermal_*`, `APerformanceHint_*`, `ASharedMemory_dupFromJava`,
  `android_getaddrinfofornetwork` & co.
- **ANativeActivity** (`android.app.NativeActivity`): `ANativeActivity_finish`/
  `setWindowFormat`/`setWindowFlags`/`showSoftInput`/`hideSoftInput` are
  dropped (one message says so), and nothing calls an app's
  `ANativeActivity_onCreate`: NativeActivity's natives
  (`android_app_NativeActivity.cpp`) are not built, and an Activity has no
  window of its own under gesher-app yet (the graphics track's
  IWindowSession/ViewRootImpl work). The next step, once Activity windows
  exist: build those natives; `loadNativeCode` dlopens the app's library
  (through the ELF dlopen hook) and calls `ANativeActivity_onCreate` through a
  System V stub like the JNI bridge's, with `vm`/`env` the bridge's proxies;
  the `callbacks` the `.so` fills are System V and must be called as such,
  with `SetupThreadTls` on the thread; `onNativeWindowCreated` gets the
  Activity's Surface as an `ANativeWindow` (a `Win32Surface`, so
  `ANativeWindow_lock` and `eglCreateWindowSurface` work on it as tested
  here), plus `AInputQueue` for input.
- **Graphics limits**: EGL window surfaces need a Gesher window (a
  `Win32Surface`, found by `gesher_window_hwnd`); other `ANativeWindow`s give
  `EGL_BAD_NATIVE_WINDOW`; pixmap surfaces are passed through to ANGLE and
  fail. An ES 2 context is ANGLE's ES 3.0 (backward compatible, as Android
  drivers may do). GLES 1 (`libGLESv1_CM.so`) resolves but is untested.
  `AChoreographer`, `ASurfaceControl`, `AHardwareBuffer`, `ASurfaceTexture`
  are not provided. `eglDestroySurface` of a surface that is still current
  releases the window at once (Android defers it).
- **The test APK** is built against the public SDK's `android.jar`
  (platform 36 r2), not Gesher's framework classes; its native code is API 26
  (emulated TLS) and its `libc++_shared.so` is the NDK's.
- The stack cookie is the TEB address: correct, not secret.
- Un-rewritable `%fs` forms (other TCB displacements, RIP-relative, VEX with a
  register-based `%fs` operand, partial writes of the base register) load as
  `int3` sites that abort with a message if reached; none occur in the test
  libraries.
- Shim gaps left: `fork`/`exec*`/`posix_spawn`/`popen`/`system`, `getopt`,
  `getpwnam`/`getpwuid`, `dl*` namespaces, `wordexp`/`glob`, `iconv`,
  `strptime`, `wprintf`/`fwprintf`/`vfwprintf`/`fgetws`/`wscanf`,
  wide-character `%ls` in `scanf`, and `__system_property_get` (every
  property reads as ""). `fcntl` record locks always succeed (not modelled).
  `environ` is the environment as the shim found it. Missing **function**
  imports load as stubs that print the name and abort when called; missing
  **data** imports load as inaccessible placeholders that name the symbol if
  touched.
- Threads ART creates (not through the shim's `pthread_create`) do not run
  bionic key destructors or `thread_local` destructors when they exit.
- `__system_property_get` returns "" for everything; `__android_log_*` writes
  `P/tag: text` to stderr.
- Closing a library does not unmap it or release its TLS offset.

### After that (not started)

- `ANativeActivity` once Activities have windows (above), and the remaining
  shim gaps as real apps need them.
- Faults in loaded code as `SIGSEGV`/`SIGBUS` to the `.so`'s own handlers
  (crash reporters), chained with ART's fault handling; asynchronous delivery
  to a running thread (suspend, redirect to a handler trampoline, resume).
- Real-world APKs through gesher-app: the NDK samples (hello-jni,
  hello-libs), and C++ code registering natives from tables.
- A correction to this file's earlier plan: JNI was not "System V on both
  sides already". ART's `JNIEnv`/`JavaVM` tables are Microsoft ABI
  (`GESHER_JNI` only wraps natives the framework registers); the bridge's
  proxies are what make a `.so`'s `JNIEnv` calls work
  ([0017](decisions/0017-native-libs-jni-bridge.md)).

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
