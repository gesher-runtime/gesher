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
Android NDK on the Linux runner and uploads them as the `native-libs`
artifact; the Windows `test` job loads them through `elf_load_test` (and later
through gesher-app).
