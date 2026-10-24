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

- `--compiler-filter=verify`: no compiled code (the code generator does not
  follow Gesher's thread register yet, ADR 0006); classes are verified and
  linked, and initialized where dex2oat can do it at compile time.
  `--image-format=lz4`, `--dex-location=<jar name>` (the image loads wherever
  the jars are), `--base=0x60000000`.

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
- [ ] Built in CI, loaded by every test run and by the installed Gesher
- [ ] Startup measured in CI, imageless and with the image

Not done yet:
- Compiled code in the image (AOT), the JIT and nterp: they need the code
  generator to reach Thread* the way Gesher's assembly does (ADR 0006).
- Sharing image and oat pages between processes (placeholder mappings);
  each process has its own copy now.
- An app image for the app's own classes.
