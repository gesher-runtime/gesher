# Gesher

**Run Android apps on Windows as native Windows processes — no emulator, no virtual machine.**

Gesher (Hebrew: *bridge*) is to Android what Wine is to Windows: a compatibility
layer. Double-click an `.apk`, and the app runs in an ordinary window, at native
speed, with no hidden VM and no Android image to boot.

> **Status: phase 1 — porting ART.** The ART runtime builds for Windows and `dalvikvm.exe` starts; it does not run Java code yet, and nothing runs apps yet.
> See the [roadmap](docs/roadmap.md).

## How it works

Instead of rewriting Android, Gesher compiles the Android Open Source Project's
own runtime and framework for Windows and replaces only the layers that talk to
the Linux kernel:

```
 .apk ─► APK loader       manifest, resources, dex, native libs
         ART              Android's own Java runtime, ported to Windows
         framework.jar    the real Android framework
           └ system services re-implemented in-process
         HWUI + Skia      → OpenGL ES → ANGLE → Direct3D 11
         native .so       ELF loader + bionic on Win32 (+ arm64 translation)
         integration      windows, input, IME, clipboard, files, notifications
```

Full design: [docs/architecture.md](docs/architecture.md).

## Principles

- **No VM, no emulator.** One Windows process per app.
- **Light.** Nothing runs when no app is open.
- **No store.** Gesher opens `.apk` files the user already has.
- **Respects the host.** Networking and TLS use Windows' own certificate
  store, so content filters and corporate proxies work unchanged.
- **CI does the heavy lifting.** Every build and test runs on GitHub Actions;
  contributors don't need a powerful machine.

## Building

Requirements: CMake 3.24+, Ninja, Python 3, git, and
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (works on Linux and
Windows hosts). CI does all of this on every push.

```sh
tools/fetch-aosp.sh                       # pinned AOSP sources + Gesher patches
cmake -S . -B out -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-llvm-mingw.cmake \
  -DLLVM_MINGW=/path/to/llvm-mingw
ninja -C out
```

Outputs today:

- `platform_probe.exe`: checks the Windows primitives ART relies on.
- `dexdump.exe`: ART's own DEX reader, running natively on Windows.

Tests: `tests/build-test-apps.sh out/test-apps` (needs a JDK), then
`tests/windows/run-tests.ps1` on Windows.

## License

[Apache License 2.0](LICENSE), the same license as AOSP. See
[decision 0001](docs/decisions/0001-license.md).

---

## בעברית

‏**גשר** מריץ אפליקציות אנדרואיד על Windows כתוכנות רגילות, בלי אמולטור ובלי
מכונה וירטואלית. המטרה היא כלי קל שפותח קובצי ‎`.apk` בלחיצה כפולה ומכבד את
הסינון שמותקן במחשב. הפרויקט בשלב 1: מנוע ה-ART נבנה ל-Windows ועולה, אך עדיין אינו מריץ קוד.
