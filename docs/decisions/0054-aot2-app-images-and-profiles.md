# 0054 — App dexopt, JIT profiles, app images and a profile-guided boot image

**Status:** accepted, 2026-10-25 (aot2 track). Extends ADR 0031, 0033 and 0036.

## Context
After the codegen and stack tracks (ADR 0033, 0036) Gesher had nterp, the JIT
and AOT code, but not Android's *compilation model*:

- Apps were never compiled: an installed APK ran from its dex code, and the
  Gesher device's app processes ran the switch interpreter (`-Xint`), the
  options every process of the device shares.
- No profiles: the JIT's profile saver never ran, so nothing could compile an
  app with `speed-profile` and no app image (`base.art`, the app's classes
  preloaded and linked) was ever made.
- The boot image compiled every method of every jar (`speed`):
  boot-framework.oat was 75.7 MB, the installer 112 MB.

On a device, ART Service (artd, dex2oat, profman) compiles an app at install
with `pm.dexopt.install` (`speed-profile`, which is `verify` without a
profile), app processes run the JIT with its profile saver
(`dalvik.vm.usejitprofiles`), and background dexopt (BackgroundDexOptService,
when the device is idle) merges the JIT profile into the reference profile
and compiles `speed-profile` with an app image. The boot image is compiled
`speed-profile` with the boot image profiles in art/build/boot and
frameworks/base/boot.

## Decision
- **The same model on the Gesher device**, in `com.gesher.server.Dexopt`:
  - At install (`Installer.install`) the system server runs dex2oat.exe
    (next to dalvikvm.exe, a process of its own) on `base.apk` into
    `<code path>/oat/x86_64/base.{odex,vdex}`, where ART's OatFileAssistant
    looks, with the filter `pm.dexopt.install` (default `speed-profile`, so
    `verify` while there is no profile), the class loader context `PCL[]`,
    and the boot image and boot class path of the device's runtime (from its
    command line). The profile files are made as installd makes them
    (`$ANDROID_DATA/misc/profiles/{cur/0,ref}/<package>/primary.prof`).
  - App processes run with the JIT and `-Xjitsaveprofilinginfo` instead of
    the device's `-Xint -Xusejit:false` (`dalvik.vm.usejit`,
    `dalvik.vm.usejitprofiles`, default true; `GESHER_APP_EXECUTION=interpret`
    keeps the old options). The framework registers the app's code path with
    ART (LoadedApk.registerAppInfoToArt), whose profile saver writes the
    current profile.
  - Background dexopt: a system server thread waits until no app process has
    run or started for `gesher.dexopt.idle-ms` (default 2 s; Android waits for
    an idle, charging device), then, for each package whose current profile
    has new data, runs profman to merge it into the reference profile and, if
    profman says compile, dex2oat with `pm.dexopt.bg-dexopt` (default
    `speed-profile`), `--profile-file` and `--app-image-file`. It compiles
    into `oat.new` and swaps the directory in, so no run sees half a compile
    (Windows refuses to replace files a running process has mapped; a swap
    that meets one waits for the next round).
- **ART patch 0540** (patches/art/0540-app-images-and-profiles-on-windows.patch):
  - An app image's methods without compiled code point at the primary boot
    image's oat stubs (ImageWriter writes them, as for the extensions); since
    patch 0012 the boot image's methods use libart's stubs instead, so the
    class linker now gives app image methods libart's stubs too when it adds
    the image (`AddImageSpace`).
  - `System.exit` (`Runtime::CallExitHook`) saves the JIT profile first:
    Gesher's app processes exit as soon as the system server tells them to,
    usually before the profile saver's first periodic save (Android's last
    save is the periodic one before the process dies; the signal catcher
    forces one on SIGUSR1).
  - `GESHER_LOG_EXECUTION` logs, in ART's own terms, which oat file an app's
    dex file gets (`gesher: app oat: ...`: status, filter, reason), the app
    image it loads (`gesher: app image loaded: ...`: classes, methods,
    methods with compiled code) and, at exit, how the app's own methods ran
    (`gesher: app methods at exit: ...`: with AOT code; entered the
    interpreter or the JIT, by their hotness counters and the JIT code cache).
- **profman.exe** is built with dex2oat (cmake/aosp/dex2oat.cmake).
- **The boot image with AOSP's profiles**: boot-image.yml fetches
  art/build/boot/boot-image-profile.txt and frameworks/base/boot/
  boot-image-profile.txt at aosp.lock's tag (tools/boot-image/fetch-profiles.ps1),
  profman turns them into one boot profile (`--output-profile-type=boot`, as
  Soong does) and compile.ps1 compiles the primary image and framework.jar
  `speed-profile`: the profile's methods get AOT code and its classes go into
  the image. gesher-app.jar, which no AOSP profile covers, stays `speed`.
- **The installer** ships dex2oat.exe and profman.exe.

## Consequences
- See docs/compiled-code.md ("Status and next steps (aot2 track)") for the
  CI-verified numbers: sizes before and after, startup, the app's methods.
- Every app process of the device now runs nterp and the JIT, not the switch
  interpreter: the framework's code runs its AOT code where the boot profile
  compiled it.
- Not done: the class loader context is `PCL[]` (an app with shared libraries
  gets a context mismatch and runs from its APK, as ART refuses the oat file);
  no cloud profiles (`.dm` files); secondary dex files are not compiled;
  oatdump is not built (ART's logging stands in).
