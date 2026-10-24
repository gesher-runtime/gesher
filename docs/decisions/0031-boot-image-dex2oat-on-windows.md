# 0031 — Boot image: dex2oat on Windows, no compiled code

**Status:** accepted, 2026-10-24 (boot-image track)

## Context
Every dalvikvm process (`gesher`, `gesher-app`, the system server and each app
process it starts) ran "imageless": ART found no boot image under
`-Ximage:…/system/framework/boot.art` and fell back to defining, linking and
verifying every boot class it touches from the boot jars' dex files, one by
one, with the interpreter (`-Xint`). On Android the boot image (`boot*.art`,
with `.oat` and `.vdex` files) holds those classes already loaded: a heap image
of the classes, their methods and fields, interned strings and the statics of
the classes dex2oat could initialize, which the runtime maps at startup.

A boot image is made by dex2oat, ART's ahead-of-time compiler, and is tied to
the runtime that loads it: the image's layout of `ArtMethod`, `mirror::Class`
and the other native and managed structures, the GC configuration (read
barriers or not), the boot class path's checksums and dex locations.
Gesher's runtime differs from Linux ART in its ABI (ADR 0006: Thread* in a TLS
slot, not `%gs`; System V at assembly boundaries), its data model (LLP64,
patch 0002) and its platform layer (gesher-base's `mmap`).

## Options
1. **dex2oat built for the Linux host** in CI, targeting x86-64. dex2oat is a
   host tool on Android. But a Linux build of ART is a second build of the
   runtime, of everything it links, with different `gUseReadBarrier`,
   different `long` and a different platform below it: the image would be
   made by a runtime that is not the one that loads it.
2. **dex2oat built for Windows**, from the same CMake build as dalvikvm.exe and
   linked with the same libart (`cmake/aosp/dex2oat.cmake`), run on a Windows
   runner. The image is then made by exactly the runtime that loads it.
3. **No boot image, only per-app verification caches** (app image / vdex):
   little gain, since most startup time goes into the boot class path.

## Decision
- **Option 2.** `dex2oat.exe` (libart-compiler with the x86-64 code generator
  only, libart-dex2oat, dex2oat's main) is built by ci.yml's build job;
  `.github/workflows/boot-image.yml` runs it on windows-2022
  (`tools/boot-image/compile.ps1`) and ships `framework/x86_64/boot*.{art,oat,vdex}`
  as the artifact `boot-image` to the test and package jobs. dalvikvm.exe does
  not link the compiler (`compat/art/compiler_standins.cc`).
- **No compiled code: `--compiler-filter=verify`.** Compiled code would have to
  follow Gesher's managed ABI: the x86-64 code generator reads Thread* as
  `%gs:offset` in every method, which on Windows is the TEB (ADR 0006), and
  the JIT is off for the same reason. That port is separate work (the code
  generator's thread register, the math entrypoints). The image without code
  still gives what matters for startup: the classes verified, linked and, where
  possible, initialized, and their strings interned.
- **The image's trampolines are not used.** Even without compiled code, the
  oat file of a boot image holds trampolines (resolution, generic JNI, IMT
  conflict, interpreter bridge, JNI dlsym lookup) that jump through
  `%gs:offset`. `ClassLinker::InitFromBootImage` (patch 0012) points every
  boot image method that has one at libart's own stubs, which are Gesher's
  ported assembly, and uses those from then on, as it does without an image.
- **Three image chunks**, so that every boot class path in use gets as much
  as it can: the primary image for `core-oj`, `core-libart`, `core-icu4j`
  (`boot.art`, `boot-core-libart.art`, `boot-core-icu4j.art`), and one
  extension each for `framework.jar` (`boot-framework.art`) and
  `gesher-app.jar` (`boot-gesher-app.art`). ART loads the primary image and the
  extensions of the jars that follow on the boot class path; on Gesher a lone
  `-Ximage:<dir>/boot.art` implies `<dir>/*` (patch 0012), so launchers and
  tests keep naming only `boot.art`.
- **Bare dex locations.** Where Gesher is installed is unknown when the image
  is compiled, and ART checks the boot class path's dex locations against the
  image's. dex2oat runs with `--dex-location=core-oj.jar` (and bare names in
  `-Xbootclasspath-locations`); ART already gives an oat file's dex files the
  actual path when the compiled location is a bare name that matches it
  (`OatFileBase::Setup`), and patch 0012 makes the image's recorded boot class
  path match the runtime's locations by file name. The dex checksums are still
  checked, so a stale image is refused (and the runtime falls back).
- **gesher-base maps oat files by copying.** ART's ELF loader maps an oat
  file's segments with `MAP_FIXED` into a reservation it made; Windows can put
  a file view only into free address space (placeholders would be needed). A
  private `MAP_FIXED` file mapping inside one of gesher-base's anonymous
  regions gets the file's contents copied in, executable ones included. The
  `.art` files are LZ4-compressed (`--image-format=lz4`), which ART decompresses
  into anonymous memory anyway. Relocation deltas are 64 KiB aligned (ASLR of
  the image as on Linux, at Windows' allocation granularity).
- **Where it is installed.** The installer puts the image in
  `{app}\framework\x86_64`, next to the jars, and the launchers pass
  `-Ximage:{app}/framework/boot.art`. Tests copy it to
  `$ANDROID_ROOT/framework/x86_64`, where their `-Ximage` already pointed.
- **CI asserts it.** `GESHER_REQUIRE_BOOT_IMAGE=1` makes the imageless fallback
  fatal and has the runtime log `gesher: boot image loaded: N spaces (...)`;
  run-tests.ps1 sets it for every check, the package job for every run of the
  installed Gesher. `tests/windows/tracks/60-boot-image.ps1` checks what each
  boot class path loads and times startup without and with the image.

## Consequences
- Startup no longer defines the boot classes at run time (measured in CI; see
  [boot-image.md](../boot-image.md)).
- The image must be rebuilt with any change to the runtime or the boot jars;
  CI does so on every run. A mismatched image is refused by its checksums.
- Oat and image pages are private copies per process (no sharing between the
  system server and app processes yet); placeholder mappings
  (`VirtualAlloc2`/`MapViewOfFile3`) would let processes share them.
- AOT-compiled code, the JIT and nterp with an image remain to be done; they
  need the code generator to follow ADR 0006.
