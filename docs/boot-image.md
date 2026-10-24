# Boot image

ART starts fast on Android because the boot class path is already loaded when
a process starts: the boot image (`boot*.art`, with `.oat` and `.vdex` files)
holds its classes, methods, fields and interned strings, and the statics of
the classes dex2oat could initialize, which the runtime maps instead of
defining every class from the jars' dex files. Gesher builds one in CI and
ships it with the installer. The decision and its reasons are in
[ADR 0031](decisions/0031-boot-image-dex2oat-on-windows.md).

## How it is made
- `dex2oat.exe` is built with the runtime (`cmake/aosp/dex2oat.cmake`:
  libart-compiler with the x86-64 code generator, libart-dex2oat, on the same
  libart as dalvikvm.exe).
- `.github/workflows/boot-image.yml` runs `tools/boot-image/compile.ps1` on a
  Windows runner after the build and framework jobs, then runs Hello on the
  result. It writes `framework/x86_64/`:

  | files | boot class path | |
  |---|---|---|
  | `boot.{art,oat,vdex}`, `boot-core-libart.*`, `boot-core-icu4j.*` | core-oj, core-libart, core-icu4j | primary image |
  | `boot-framework.*` | framework.jar | extension |
  | `boot-gesher-app.*` | gesher-app.jar | extension |

- dex2oat's own runtime gets `-Xmx512m` and `-XX:LargeObjectSpace=map`: a
  free-list large object space is one reservation of the whole heap, which
  ART's low-4 GiB allocator searches for page by page (`msync`), and that took
  for ever on Windows with a larger heap.
- dex2oat writes the `.vdex` file through a shared mapping and, on Linux,
  cuts its page-aligned length back at the end; Windows refuses to shrink a
  mapped file (`ERROR_USER_MAPPED_FILE`), so on Gesher the file first ends with
  the dex files and what follows them is written with pwrite (patch 0013).
- `--compiler-filter=speed` for the primary image and the extensions: AOT
  code for core-oj, core-libart, core-icu4j, framework.jar and gesher-app.jar
  ([compiled-code.md](compiled-code.md), ADR 0033, 0036). (Until the codegen
  and stack tracks: `verify`, no compiled code.) `--image-format=lz4`,
  `--dex-location=<jar name>` (the image loads wherever the jars are),
  `--base=0x60000000`.

## How it is loaded
- `-Ximage:<dir>/boot.art` looks in `<dir>/x86_64/`. On Gesher a lone location
  also finds the extensions next to it, as if `<dir>/*` followed (ART patch
  0012), so each boot class path gets the primary image and the extensions of
  the jars it has, in order: Hello (3 jars) loads 3 spaces, framework.jar runs
  4, gesher-app and its apps 5.
- The image is relocated to a random 64 KiB aligned address, as ART does on
  Linux. gesher-base copies oat file segments into ART's reservation (a
  private `MAP_FIXED` file mapping inside its own anonymous memory).
- The oat file's trampolines are not used (they read Thread* through `%gs`):
  the image's methods get libart's stubs when it loads (patch 0012).
- The installer puts the image in `{app}\framework\x86_64`; `gesher` and
  `gesher-app` pass `-Ximage:{app}/framework/boot.art`. A stale or mismatched
  image (other jars) is refused by its checksums, and the runtime falls back
  to imageless running.
- `GESHER_REQUIRE_BOOT_IMAGE=1` turns that fallback into a fatal error and
  makes the runtime log `gesher: boot image loaded: N spaces (...)`.

## Status
Verified in CI (ci runs 40055199839 and 40055865380, windows-2022 and
windows-2025):
- [x] dex2oat.exe builds with the runtime and compiles the image on
      windows-2022 in under 6 s (primary 1.5 s, framework.jar 2.8 s,
      gesher-app.jar 1.2 s); Hello runs on it in the boot-image job.
- [x] Every dalvikvm run of the test jobs loads it: run-tests.ps1 sets
      `GESHER_REQUIRE_BOOT_IMAGE`, so a run that fell back to imageless running
      would fail. `tracks/60-boot-image.ps1` checks what each boot class path
      loads (Hello 3 spaces, framework.jar 4, lifecycle.apk 5).
- [x] The installer ships it and the installed `gesher` and `gesher-app`
      (device, system server, app processes) load it (package job).
- [x] Startup, from the process start, median of 3 (`tracks/60-boot-image.ps1`):

  | | imageless | with the image |
  |---|---|---|
  | Hello, to its output (windows-2022 / 2025) | 105 / 111 ms | 55 / 54 ms |
  | lifecycle.apk, to `LifecycleActivity.onCreate` | 783 / 755 ms | 483 / 419 ms |
  | lifecycle.apk, to exit | 854 / 818 ms | 530 / 461 ms |

  (run 40055199839; run 40055865380: Hello 105 / 82 -> 55 / 46 ms,
  lifecycle.apk to `onCreate` 788 / 544 -> 489 / 299 ms.)

With AOT code in every image (ci run 40138006773): dex2oat takes 2.7 s
(primary), 8.8 s (framework.jar) and 0.7 s (gesher-app.jar); the `.oat` files
are 95 MB (boot-framework.oat 75.7 MB), the boot-image artifact 64 MB
compressed. The oat files are mapped as views (ADR 0036), not read, and
lifecycle.apk's startup ratio to imageless stays about the same (0.42-0.46,
was 0.47; compiled-code.md).

Sizes before compiled code: 65 MB in all, of which 50 MB are the `.vdex` files (they carry a copy
of the dex code, which the runtime reads instead of the jars' compressed entries),
the `.art` files 14 MB (LZ4) and the `.oat` files 0.4 MB.

Not done yet:
- Tools around it: the image is not rebuilt on the user's machine (a
  `dex2oat.exe` in the installer could do that for other jars), and there is no
  oatdump.
- Compiled code in the image, the JIT and nterp are done (compiled-code.md);
  the oat files' code is mapped as views of the files, shared between
  processes (placeholder mappings, ADR 0036).
- An app image for the app's own classes.
