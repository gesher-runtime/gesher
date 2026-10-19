# Gesher

**Run Android apps on Windows as native Windows processes — no emulator, no virtual machine.**

Gesher (Hebrew: *bridge*) is to Android what Wine is to Windows: a compatibility
layer. Double-click an `.apk`, and the app runs in an ordinary window, at native
speed, with no hidden VM and no Android image to boot.

> **Status: phase 0 — research and infrastructure.** Nothing runs apps yet.
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

Phase 0 has one artifact, a probe that checks the Windows primitives ART needs:

```sh
x86_64-w64-mingw32-gcc -O2 -o platform_probe.exe probes/platform/platform_probe.c -lsynchronization
./platform_probe.exe
```

CI builds it with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) and runs
it on Windows Server 2022 and 2025.

## License

[Apache License 2.0](LICENSE), the same license as AOSP. See
[decision 0001](docs/decisions/0001-license.md).

---

## בעברית

‏**גשר** מריץ אפליקציות אנדרואיד על Windows כתוכנות רגילות, בלי אמולטור ובלי
מכונה וירטואלית. המטרה היא כלי קל שפותח קובצי ‎`.apk` בלחיצה כפולה ומכבד את
הסינון שמותקן במחשב. הפרויקט בשלב 0 (מחקר ותשתית).
