# Architecture

Gesher runs an Android app as a single Windows process. Inside that process are
Android's own runtime and framework, compiled for Windows, sitting on a thin
layer that maps Linux and Android platform services onto Win32.

```
┌──────────────────────────── app.exe (one per running app) ───────────────────────────┐
│  app code: classes.dex                    app native code: lib*.so                    │
│ ─────────────────────────────────────    ──────────────────────────────────────────── │
│  framework.jar  (AOSP, unmodified where   ELF loader ─ bionic-on-Win32                │
│  possible)                                └ arm64 → x86-64 translator (phase 7)       │
│  in-process system services:                                                          │
│   ActivityManager, WindowManager,                                                     │
│   PackageManager, InputMethod, ...                                                    │
│ ─────────────────────────────────────                                                 │
│  libcore (java.*) + Conscrypt (TLS)  ──► Windows cert store                           │
│  ART runtime  (AOSP, ported)                                                          │
│  libandroid_runtime, HWUI, Skia  ──► OpenGL ES ──► ANGLE ──► Direct3D 11              │
│ ───────────────────────────────────────────────────────────────────────────────────── │
│  gesher-base: the Linux-isms ART and friends need, on Win32                           │
│   mmap/mprotect → VirtualAlloc/MapViewOfFile   futex → WaitOnAddress                  │
│   SIGSEGV → vectored exception handlers         /proc → synthesized                   │
└───────────────────────────────────────────────────────────────────────────────────────┘
```

## Components

### 1. APK loader
Opens the `.apk` (a zip), parses the binary `AndroidManifest.xml` and
`resources.arsc`, picks the right native-library ABI, and prepares a per-app
data directory under `%LOCALAPPDATA%\Gesher\<package>`. Registered as the
handler for `.apk` files.

### 2. ART
The Android Runtime from AOSP. ART already builds and runs on Linux hosts (for
tests and host tools); Gesher adds a Windows host target. Main porting points,
each validated by [`probes/platform`](../probes/platform/platform_probe.c):

| ART needs | Linux mechanism | Windows equivalent |
|---|---|---|
| implicit null checks | `SIGSEGV` handler rewrites PC | vectored exception handler rewrites `Rip` |
| stack overflow checks | guard page + `SIGSEGV` | guard page + `SetThreadStackGuarantee` |
| locks, monitors | `futex` | `WaitOnAddress` / `WakeByAddress*` |
| compressed references | heap below 4 GiB via `mmap` hint | `VirtualAlloc` at chosen low address |
| boot image, `.oat` files | `mmap` of file at fixed address | `MapViewOfFileEx` |
| JIT code cache | `mprotect` RW↔RX, dual mapping | `VirtualProtect`, `MapViewOfFile3` |

The scale of the remaining work is tracked by the
[AOSP survey](../.github/workflows/aosp-survey.yml), which counts every
Linux-specific API use in the projects we port.

### 3. Framework and system services
On a phone, an app talks to `system_server` over Binder. Gesher runs the real
`framework.jar` and replaces the remote services with in-process
implementations, the same approach Robolectric uses to run the framework on a
JVM. Only the services an app actually touches are implemented, and missing
pieces fail loudly so they can be prioritised.

### 4. Graphics
HWUI and Skia render through OpenGL ES. On Windows, OpenGL ES is provided by
[ANGLE](https://chromium.googlesource.com/angle/angle) over Direct3D 11, the
same stack Chrome uses. Games that use GLES or Vulkan directly reach the same
backends.

### 5. Native libraries
Many apps ship `.so` files. Gesher loads ELF shared objects itself and links
them against a Windows implementation of bionic (Android's libc). x86-64
libraries run directly; arm64-only apps need dynamic binary translation
(phase 7), following the approach of Box64 and FEX.

### 6. Windows integration
Each Activity task becomes a top-level window. Mouse and pen become touch
events, the keyboard and Windows IME (including Hebrew and RTL) feed Android's
input method, and clipboard, file pickers (Storage Access Framework),
notifications (Windows toasts), audio (WASAPI) and media codecs (Media
Foundation) are bridged. `WebView` is implemented on top of Microsoft Edge
WebView2, which ships with Windows 10 and 11.

## Networking and certificates
Android apps trust only system certificate authorities by default. Gesher
defines the system store as Windows' own trusted roots, so anything the user's
network requires (content filters, corporate TLS inspection) works with no
extra setup.

## What Gesher does not do
- Provide an app store or Google Play.
- Ship Google's proprietary apps or Play Services. microG may be integrated
  later for apps that depend on Play Services APIs.
- Run apps that need kernel drivers or root.

## Desktop script responsibilities

All desktop scripts remain in `tools/package` and are installed by `gesher.iss`.

| File | Responsibility |
| --- | --- |
| `gesher-control-panel.ps1` | WPF presentation, refresh, action admission/completion, update confirmation and close policy. |
| `gesher-panel-actions.ps1` | APK error propagation and background execution, including ConsoleHost/native handle ownership. |
| `gesher-panel-native.ps1` | Native desktop and metadata monitoring helpers. |
| `gesher-app.ps1` | Application/device command entry point and runtime/profile orchestration. |
| `gesher-app-microg.ps1` | microG staging and enable/disable operations. |
| `gesher-update.ps1` | Release discovery, verified download and installer process helpers. |
| `gesher-runtime-files.ps1` | Runtime-owned file synchronization into the Android profile. |
| `gesher-ui.ps1`, `ui-messages.json` | Language selection and message catalog. |
| `gesher.iss` | Per-user installation, shell integration and optional components. |

The panel dot-sources the action module at the former definition site, supplying
`$here` (installed script directory) and the selected UI language.
`New-PanelActionWorker(operation, request)` returns the existing
`PowerShell`, `Handle`, `Disposed` record; its result contains `Errors`. The panel
owns admission, polling, disposal and feedback. Workers receive data, never WPF
controls, retaining helper ordering, exit codes and temporary-file cleanup.
A committing install is not stopped on close.

Update orchestration stays with confirmation/rendering in the panel; transport
and installer helpers already live in `gesher-update.ps1`. Further splitting
should follow responsibility rather than line counts.
See [Build and package](build-and-package.md) for derived artifacts and validation.
