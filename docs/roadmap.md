# Roadmap

Each phase ends with something demonstrable, built and tested in CI.

| Phase | Goal | Done when |
|---|---|---|
| **0** | Research and infrastructure | CI cross-compiles with llvm-mingw and tests on Windows; Windows primitives for ART validated; porting surface measured |
| **1** | ART on Windows | `dalvikvm` runs a `Hello World` `.dex` from `cmd.exe` |
| **2** | Headless framework | an app's Activity goes through its lifecycle with resources and manifest loaded |
| **3** | Graphics and input | a simple Activity is drawn in a Win32 window via ANGLE and responds to clicks |
| **4** | First real apps | pure-Java apps work end to end; fonts, Hebrew/RTL; `.apk` file association |
| **5** | Network and web | TLS against the Windows cert store; `WebView` via WebView2 |
| **6** | Native x86-64 libraries | ELF loader and bionic-on-Win32 |
| **7** | arm64 translation | arm64-only apps run on x86-64 PCs |
| **8** | Everyday use | media, audio, notifications, microG, installer |

## Phase 0 checklist

- [x] License chosen ([0001](decisions/0001-license.md))
- [x] Toolchain chosen ([0002](decisions/0002-toolchain.md))
- [x] CI-first build strategy ([0003](decisions/0003-ci-first.md))
- [x] Platform probe: implicit null checks, stack overflow, futex, low-4 GiB
      heap, fixed-address file mapping, JIT
- [x] AOSP porting-surface survey, pinned to `android-16.0.0_r4`

## Phase 1 checklist

- [x] Build approach decided: CMake mirroring `Android.bp`, with overlays and
      patches ([0004](decisions/0004-build-system.md))
- [x] libbase, liblog, libziparchive, zlib, tinyxml2, fmtlib for Windows
- [x] libartpalette, libartbase, libdexfile for Windows
- [x] `dexdump.exe` reads and disassembles `.dex` files, including from a
      compressed `.apk`, tested in CI on Windows 2022 and 2025
- [x] gesher-base POSIX layer started ([0005](decisions/0005-posix-layer.md)):
      `mmap` family (anonymous maps, hints, partial unmap, `mprotect`,
      `madvise`), signal sets and handler registration; Linux-layout
      `pid_t`, `siginfo_t`, `ucontext_t`; headers for `dlfcn`, `elf`,
      `link`, `resource`, `uio`, `utsname`, `statvfs`, `poll`, `unistd`
- [x] LLP64: ART treats Windows x64 as 64-bit (patch 0002)
- [x] Generated sources outside Soong: aconfig flags, `asm_defines.h`
      (computed for the Windows target), enum operators
- [x] **ART runtime compiles for Windows**: all 237 files of libart-runtime,
      plus libelffile, libprofile, libnativebridge, libnativeloader, liblzma,
      lz4, cpu_features
- [x] Implement the declared POSIX functions (`dlopen`,
      `sysconf`, `getrlimit`, `poll`, `uname`, futex via `syscall`, ...)
- [x] **`dalvikvm.exe` links and starts** (`-showversion`), tested in CI on
      Windows 2022 and 2025. JIT compiler and native unwinding are stubbed
      for now (`compat/art/`)
- [x] Calling convention and thread register decided
      ([0006](decisions/0006-managed-abi.md)); gesher-base keeps `Thread*` in
      a TLS slot reachable through `%gs`
- [x] Assembly builds as COFF (nterp generated, `.type`/`.size`/`.hidden`/PLT
      handled); `Thread*` loads ported
- [x] Every thread-register access in the assembly (`%gs:FIELD`, nterp's
      `rSELF`) goes through the TLS slot; the `asm-port` CI job keeps it at 0
- [x] `sysv_abi` (`ART_SYSV_ABI`) on every C++ function the assembly calls
      and on the assembly C++ calls; C library calls go through System V
      wrappers. `tools/abi-check.py` verifies all 203 boundaries in CI
- [ ] Math entrypoints (`sin`, `fmod`, ...) called by compiled code need
      System V wrappers too (the Microsoft ABI's 32-byte shadow space);
      only matters once the JIT/AOT compiler runs
- [x] Natives entered with the System V ABI: `JNICALL` is `sysv_abi`, and
      `jni_macros.h` registers System V thunks; the `JNIEnv`/`JavaVM` tables
      stay on the Microsoft ABI ([0011](decisions/0011-libcore-natives.md))
- [x] Android root and data directories from the environment (patch 0008)
- [x] Boot class path built from libcore in CI: `core-oj`, `core-libart`
      ([0007](decisions/0007-boot-class-path.md))
- [x] Real signal chain (libsigchain) on gesher-base; ART uses explicit
      null, suspend and stack-overflow checks until signals are delivered
- [x] dlmalloc in its POSIX configuration, so the non-moving space stays in
      ART's low-4 GiB heap
- [x] `dalvikvm.exe` links the core classes from the boot jars without a boot
      image (tested in CI)
- [x] Crash reports: unhandled exceptions print a stack trace
      (`tools/symbolize.py`, or `addr2line` on an unstripped build);
      `GESHER_TRACE_MMAP=1` traces the mmap layer
- [x] Linux errno numbering, translated from the CRT's and winpthreads'
      ([0008](decisions/0008-linux-abi.md))
- [x] Sockets as file descriptors over Winsock, `poll` over files, pipes and
      sockets, with a probe run in CI ([0009](decisions/0009-socket-fds.md))
- [x] libcore's native libraries (`libicu_jni`, `libjavacore`,
      `libopenjdk`) linked in and registered with `dlopen` by name
      ([0011](decisions/0011-libcore-natives.md)), all three `JNI_OnLoad`s
      run (tested in CI)
- [x] ICU4C built from `external/icu`, its data mapped from
      `ANDROID_I18N_ROOT` as on Android; `libicu_jni`, libcore's ICU natives
      and `core-icu4j.jar` on the boot class path
- [ ] Time zone data (`ANDROID_TZDATA_ROOT`, libcore's `tzdata`)
- [ ] BoringSSL (`NativeBN`), epoll and inotify, a process model
- [x] Path model ([0010](decisions/0010-paths.md)): Linux paths are
      drive-less paths on the current drive, so ART's `:`-separated lists
      need no drive letters; `/dev/null`, `/dev/zero`, `/dev/urandom` and
      `/proc/self/cmdline` are emulated; `open` takes Linux's `O_*` flags
      over `CreateFileW`; tested in CI
- [ ] Paths on another drive in `:`-separated lists, and the rest of the
      CRT's path functions (`fopen`, `unlink`, `rename`, `access`)
- [x] Signals sent to the process wait for `sigwait`/`sigwaitinfo`, as
      ART's signal catcher takes them
- [ ] Signal delivery from Windows exceptions (VEH → `sigaction` handlers,
      `CONTEXT` ↔ `ucontext_t`), then implicit checks back on
- [ ] `MAP_FIXED` over existing reservations (placeholder mappings)
- [ ] Executable file mappings (`.oat` code)
- [ ] ART's own gtests for libartbase/libdexfile running on Windows
- [x] `dalvikvm.exe` runs `Hello` with the interpreter (tested in CI)
- [x] A boot image ([boot-image.md](boot-image.md), [0031](decisions/0031-boot-image-dex2oat-on-windows.md)):
      dex2oat.exe, built for Windows on the same libart, compiles the boot class
      path in CI (primary image and the framework.jar and gesher-app.jar
      extensions, no compiled code); every test run and the installed Gesher
      load it, with no imageless fallback; startup Hello 105 -> 55 ms,
      lifecycle.apk to `Activity.onCreate` 783 -> 483 ms (ci run 40055199839)
- [x] Compiled code on Windows ([compiled-code.md](compiled-code.md),
      [0033](decisions/0033-codegen-compiled-code-on-windows.md)): nterp, the
      JIT and AOT code in the boot image's primary image, the x86-64 code
      generator reaching `Thread*` through a fixed TLS slot, faults delivered as
      signals. Stress (exceptions, GC, threads, JNI through compiled frames) and
      a subset of ART's run-tests pass under the switch interpreter, nterp, AOT
      and the JIT; a CPU-bound benchmark runs about 2.5x (nterp) and 5-6x (JIT)
      the switch interpreter. Remaining: deep-recursion `StackOverflowError`
      under nterp/JIT (needs an alternate signal stack), compiled JNI stubs,
      AOT for the extensions, dual-mapped JIT cache.
- [x] A trial installer from CI (`tools/package`, Inno Setup): per user, a
      `gesher` command for `.dex` programs, installed and tried on a clean
      runner

## Phase 2 checklist

Approach: [0012](decisions/0012-headless-framework.md); what framework.jar
needs: [framework-build.md](framework-build.md).

- [x] `framework-res.apk`, and its `R` and `Manifest`, built with aapt2
      (`tools/build-framework-res.sh`, with release BP4A's flag values)
- [x] The framework's dependency graph mapped (`tools/gen/bp_deps.py`)
- [x] aconfig flag classes as `java_aconfig_library` makes them
      (`Flags`, `FeatureFlags`, `FeatureFlagsImpl`)
- [x] Host tools built in CI: hidl-gen, protoc's javanano and javastream
      plugins, stats-log-api-gen, sysprop, xsdc, systemfeatures
      (`.github/workflows/framework.yml`, cached and shared across branches)
- [x] Code generation for the framework: AIDL, HIDL, protos, statslog,
      sysprop, XSD, event log tags
- [x] `framework.jar` compiles (javac, no errors) and is dexed (D8), in CI
- [x] `framework.jar` on the boot class path, tested on Windows (FrameworkSmoke)
- [x] `epoll` and `eventfd` in gesher-base, for `Looper`
      (`probes/posix/epoll_probe.cc`)
- [x] libutils, libcutils and libandroidfw in full, with AOSP's gtests on
      Windows (ci run 39956255117; exclusions in `docs/core-libs.md`)
- [x] `libandroid_runtime`'s host natives linked in: assets, resources,
      `Log`, `SystemProperties`, `MessageQueue`, `Parcel`, `Binder`
- [x] In-process stand-ins for the services the lifecycle calls
- [x] `gesher-app <app.apk>`: an `Activity` reaches `onResume` with its
      resources and manifest loaded (tested in CI, and through the installer)

## Phase 3 checklist

- [x] Skia for Windows (CPU raster first), built in CI (ci run 39953186255:
      skia_probe on windows-2022 and windows-2025)
- [x] hwui's natives on Skia (Canvas, Paint, Path, Matrix, RenderNode,
      Bitmap) in place of the Phase 2 stand-ins (ci run 39999789786:
      android.graphics on framework.jar and HardwareRenderer into a Win32
      window, both runners; lifecycle.apk still reaches onResume)
- [x] ANGLE: EGL and GLES over Direct3D 11, built in CI (ci run
      39990699626: angle_probe on windows-2022 and windows-2025, D3D11 and
      WARP; docs/angle.md)
  - [x] hwui's SkiaGL pipeline (Skia's Ganesh GL) over ANGLE, chosen with
        `GESHER_HWUI_RENDERER`/`debug.hwui.renderer` = `skiagl` (the CPU
        pipeline stays the default and the fallback): HardwareRenderer draws a
        RenderNode tree with text and a bitmap into a Win32 window through an
        EGL window surface, the same frame as the CPU pipeline (ci run
        40047748570, both runners, WARP; ADR 0030)
- [x] A Win32 window as the Activity's surface (`ANativeWindow`), opened
      through a window manager stand-in (ci run 40048707298: the window
      session in the drawing process, ADR 0029; single process and an app
      process of the Gesher device)
- [x] A simple Activity drawn in a Win32 window, tested in CI by reading
      back its pixels (ci run 40048707298: uiapp.apk's layout and text drawn
      by hwui's CPU pipeline, read back with PrintWindow, both runners and
      the installer; not through ANGLE/WARP yet)
- [x] Input: Win32 mouse and keyboard messages delivered as `MotionEvent`
      and `KeyEvent`; a click reaches `onClick`, tested in CI (ci run
      40056527378: uiapp.apk, single process, the Gesher device and the
      installer)
  - [x] libinput's transport and the framework's input natives (InputChannel,
        InputEventReceiver/Sender, KeyEvent, MotionEvent, KeyCharacterMap with
        Generic.kcm, VelocityTracker); a Gesher window's mouse, wheel and key
        messages (and touch, injected) to a Java InputEventReceiver, finished
        (ci run 40038368343, both runners; docs/input.md)
  - [x] A click in an app's window reaches `onClick` (the window session
        attaches its InputChannel: `WindowInput.attach`); keys and a WM_CHAR
        reach a focused EditText, the wheel scrolls a ScrollView, the user
        resizes the window (at least its minimum) and it redraws at the new
        size (ci run 40056527378, both runners and the installer;
        docs/graphics.md)
- [x] Dialogs, menus, dropdowns and toasts as owned popup windows of the app
      window (ADR 0040; popups track): an AlertDialog over its activity with
      the dim behind it, its button, Esc and a click outside; a Toast; an
      options menu item; a Spinner selection; a dialog-themed Activity; a
      second Activity taking the task window's place (ci run 40129561197,
      both runners, single process and the Gesher device; the e2e apps stay
      green)
- [x] Fonts through minikin (bundled or Windows fonts), Hebrew and RTL text
  - [x] Bundled system fonts (Roboto, Noto Serif, DroidSansMono, Noto Sans
        Hebrew; `fonts.xml` in `$ANDROID_ROOT/etc`) as the system font map
        through SharedMemory: `Typeface.DEFAULT`, families and aliases,
        Hebrew through the fallback chain drawn right to left, a Hebrew
        `StaticLayout` paragraph, bold/italic; shipped by the installer and
        loaded by the installed gesher-app (ci run 40011630189, both runners;
        docs/fonts.md)
  - [x] RTL text in a View (TextView in a window) and further scripts:
        Hebrew and Arabic TextViews drawn right to left (bidi runs with Latin
        and digits in order, Arabic joined), alignment by gravity,
        textDirection and textAlignment, layouts mirrored under an RTL locale
        (`android:supportsRtl`), arrow keys moving an RTL EditText's cursor
        visually; Arabic, Thai, Devanagari bundled (Noto), CJK, color emoji,
        symbols and further scripts from Windows' fonts in place; the locale
        list, layout direction and regional preferences from Windows (or
        `$GESHER_LOCALES`); single process and the Gesher device (ci run
        40133992911, both runners and the installer; docs/fonts.md, ADR 0044)

## Phase 4 checklist

- [x] Binder across processes: `/dev/binder` over Windows IPC, with
      `servicemanager`
- [x] System services across processes in place of gesher-app's in-process
      stand-ins: Gesher's own system server behind the framework's AIDL
      interfaces and client side, with the framework's PackageParser2 (by
      design not services.jar; ADRs 0019, 0046; the sub-boxes' ci runs)
  - [x] The APK parsed by the framework's PackageParser2 (ci run 39984218373)
  - [x] A system server process registering package, activity,
        activity_task, window, display and user with servicemanager;
        `gesher-app --system-server` uses them across processes, and the
        launcher starts binderd, servicemanager and the server (ci runs
        39984218373, 39986921774)
  - [x] The app attached to the server (`attachApplication`,
        `bindApplication`, `ClientTransaction`s), the server driving the
        lifecycle to onResume and finishing it (pause, stop, destroy) in
        `gesher-app --system-server` (ci run 40001194804)
  - [x] The rest of the app model through the server: activities starting
        activities (results, standard, singleTop), started, bound and
        foreground services, registered and manifest broadcast receivers,
        the app's content providers and a settings stand-in (ci run
        40007889523)
  - [x] A persistent Gesher device: one system server shared by every
        launch (started detached by the first, found under a mutex, stopped
        by `gesher-app shutdown`, when idle or with its job), installed
        packages (`install`, `uninstall`, `list`), app processes the server
        starts on demand and ends; another package's activity and provider
        in its own process; `gesher-app my.apk` and the `.apk` association
        use it (ci run 40019524773)
- [x] Device-state services backed by Windows (devsvc track, ADR 0038,
      docs/services.md "Device services"; ci run 40137762701
      at a02e84a, both runners)
  - [x] Alarms that fire (a waitable timer; the app's process started for a
        PendingIntent), TIME_SET from WM_TIMECHANGE
  - [x] JobScheduler: JobServices in the app's process, periodic jobs,
        constraints from Windows
  - [x] The clipboard linked with Windows' both ways (text, HTML)
  - [x] Notifications as toasts of the app's AppUserModelID (cancel removes
        them), a notification listener
  - [x] Wake locks as Windows power requests; the battery from
        GetSystemPowerStatus
- [x] Device hardware APIs backed by Windows (devices track, ADR 0043,
      [devices.md](devices.md); ci run 40142590418 at f62f1aa, both runners;
      real cameras and sensors not in CI, see devices.md)
  - [x] Location: LocationManager over Windows.Devices.Geolocation
        (gps/network/fused/passive, listeners and PendingIntents, test
        providers, permissions; no geocoder)
  - [x] Sensors: SystemSensorManager over Windows.Devices.Sensors (empty on
        the runners; a test accelerometer and light sensor)
  - [x] Camera2 over Media Foundation (characteristics, sessions, YUV and
        JPEG ImageReaders, CAMERA permission), checked with a test camera
        that is a Media Foundation media source
- [x] `.apk` file association from the installer (per user; see
      `docs/installer.md`), tested in CI by the package job
- [x] Installed apps in the Start menu: a shortcut per launchable package
      (`Gesher Apps\<label>.lnk`, `gesher-app launch <package>`) with the
      app's label and launcher icon (PNG/WebP mipmaps, adaptive icons, the
      framework's default) as a multi-size `.ico`, and an AppUserModelID the
      app's processes set too; removed by `gesher-app uninstall` and with
      Gesher; Gesher's own icon for Setup, its entries and `.apk` files
      (ci run 40024847287; docs/installer.md, ADR 0028)

- [ ] Real apps end to end ([e2e.md](e2e.md), ADR 0034): the installer runs
      pinned F-Droid apps in CI (`e2e` job: install, first frame not blank,
      input changes the window, close ends the process; the `.apk`
      association's command and a Start menu shortcut)
  - [x] RPN calculator, Editor, Currency, NoNonsense Notes and Unit
        Converter Ultimate pass; the `.apk` association and Start menu
        shortcut start RPN (ci run 40077182991)
  - [x] What they needed: SQLite and android.database's natives,
        PropertyValuesHolder's, power/alarm/clipboard/appops/appwidget/search
        stand-ins, a no-network ConnectivityManager, a shared storage volume,
        app-compat changes by target SDK, system features, startActivities,
        PendingIntents, SdkExtensions, a SecureRandom, no sensors, a
        libphonenumber placeholder
  - [x] Simon Tatham's Puzzles (its generator is an ELF executable run as a
        process)

## Phase 5 checklist

- [x] `WebView` via WebView2 ([webview.md](webview.md), ADR 0041; limits there)
  - [x] The WebView2 SDK (NuGet, pinned) with llvm-mingw, the static loader
        linked; a controller in a child window, a page's script calling a host
        object synchronously, ExecuteScript, CapturePreview, PrintWindow with
        PW_RENDERFULLCONTENT, requests answered by the host (webview run
        40128694232: the probe on windows-2022 and windows-2025)
  - [x] android.webkit.WebView in an app's window: Gesher's
        WebViewFactoryProvider, the WebView2 window kept at the view's bounds,
        WebViewClient/WebChromeClient, evaluateJavascript,
        @JavascriptInterface, file:///android_asset, cookies, views over the
        WebView cut out of its window (single process and the Gesher device;
        ci run 40138367211 on windows-2022 and windows-2025)
  - [x] A real app that relies on WebView in the e2e job (Notes 1.41, ci run
        40138367211)

## Phase 6 checklist

- [x] ELF loader for x86-64 Android `.so` files on Windows
- [x] bionic on Win32: libc, libm, libdl, liblog and the NDK's libandroid
      over gesher-base (libandroid's non-graphics APIs)
- [x] `System.loadLibrary` of an app's NDK library from its APK's
      `lib/x86_64`, with JNI calls both ways, tested in CI
- [x] The NDK's graphics from an app's `.so`: `ANativeWindow_*` and EGL/GLES
      (libEGL/libGLESv1_CM/libGLESv2/libGLESv3 over ANGLE) on a Win32
      window, read back in CI on D3D11 and WARP (ci run 40013029994;
      docs/native-libs.md step 7)
- [x] `android.app.NativeActivity` (android_native_app_glue): an app with no
      Java code in its Activity's window, GLES 2 and `ANativeWindow_lock`,
      AChoreographer, a click and a key through AInputQueue, single process
      and on the device (ci run 40056663098; docs/native-libs.md step 8,
      ADR 0032)
