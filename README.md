# Gesher

**Run Android apps on Windows using Android's runtime and framework, with native desktop windows.**

Gesher (Hebrew: *bridge*) is a compatibility runtime. It ports AOSP ART and the
Android framework to Windows and connects them to Win32, Windows graphics and
host services. It runs Android code in Windows processes without booting a
guest Android operating system or an emulator image.

ART runs Java/DEX code, and selected real APKs run with their Android user
interfaces and native libraries. This is an actively developed compatibility
runtime; support for one app or API does not imply support for every Android app.

## Release status

**Gesher 0.1.0 is being prepared; it has not been published or release-verified.**
The final combined source must pass full CI, installer checks and both real-app
e2e suites before promotion. Publication must then verify the released bytes
again. See the [release procedure](docs/releases/README.md) and
[release preparation status](docs/handoff/tracks/release1.md).

The intended installer is `gesher-setup-0.1.0.exe`, accompanied by `SHA256SUMS`
and `gesher-0.1.0-manifest.json`. A download link, installer digest and final
source/CI references will be added only after the actual release is published
and verified. Ordinary CI installers use a different build version and are
not the 0.1.0 release.

Setup targets Windows 10 or later on x64. Runtime and installer tests run on
GitHub-hosted Windows 2022 and Windows 2025; those runners do not establish
coverage of every consumer Windows version, GPU or physical device. The
installer is unsigned. WebViews require Microsoft Edge WebView2 Runtime;
Setup offers its installation when absent.

## Running apps

The per-user installer supplies `gesher-app`, the runtime, framework, compiler
tools and shell integration. Administrator rights are not required. APKs can
be opened through Gesher using **Open with**, or from a new terminal:

```powershell
gesher-app my.apk
```

This installs and launches the APK on a persistent Gesher device, with each
app in its own process. Installed apps receive Start menu shortcuts. The
device stops after its idle timeout, or explicitly with `shutdown`:

```powershell
gesher-app install my.apk
gesher-app list
gesher-app launch com.example.app
gesher-app uninstall com.example.app
gesher-app shutdown
```

The default install directory is `%LOCALAPPDATA%\Programs\Gesher`. App data
and logs are under `%LOCALAPPDATA%\Gesher`. See [installer behavior and
cleanup](docs/installer.md) and [launcher options](app/README.md).

## Implemented and tested areas

- AOSP ART executes DEX with interpreter, JIT and AOT paths. The installation
  includes `dalvikvm`, `dex2oat`, `profman` and `oatdump`; compilation/profile
  contracts are documented in [compiled code](docs/compiled-code.md).
- Android Activities, widgets and framework services run in native Windows
  windows. HWUI/Skia draw Android content, with ANGLE providing EGL/OpenGL ES
  over Direct3D. See [graphics](docs/graphics.md) and [services](docs/services.md).
- Supported x86-64 Android ELF libraries and executables use the bionic/ELF
  compatibility layer. Selected arm64 libraries use translation. See
  [native libraries](docs/native-libs.md) and [process tools](docs/exec.md).
- Supported networking/TLS paths use Windows networking and trust integration;
  WebViews use WebView2. Exact proxy, API and accessibility boundaries are in
  [networking](docs/network.md) and [WebView](docs/webview.md).
- CI exercises installation, actual app windows and input, app shortcuts and
  file associations, and uninstall cleanup. The pinned real-app suite includes
  text/widget apps, Simon Tatham's Puzzles and TIC-80; see the [e2e scope and
  evidence](docs/e2e.md).

These are scoped implementation and test results, not a claim that the pending
0.1.0 candidate has passed its final release gates. The [roadmap](docs/roadmap.md)
and track handoffs distinguish verified work from remaining work. In particular,
current Windows4 Android IME/composition, shared display and legacy resize
corrections still need their full-suite result. Physical touch keyboard,
Wi-Fi/VPN/camera/sensor hardware and actual screen-reader speech are not
established by hosted CI. API and hardware limitations remain documented at
the corresponding subsystem links.

## How it works

```text
APK -> APK loader and Android framework services
       ART: interpreter, JIT and compiled code
       HWUI/Skia -> Win32 windows and ANGLE/Direct3D
       Android native ELF -> bionic compatibility / arm64 translation
       Host integration -> Windows input, files, networking and services
```

The source is pinned to `android-16.0.0_r4`. See the
[architecture](docs/architecture.md), [AOSP lock](aosp.lock) and
[design decisions](docs/decisions).

## Building and verification

GitHub Actions builds the pinned AOSP sources and Gesher with llvm-mingw and
`-Werror`, then runs the Windows suites, installer smoke and real-app e2e.
Use the repository's [CI workflow](.github/workflows/ci.yml) for complete
validation. C/C++/Java compilation and native/JVM fixtures on the project workstation
run only in CI; source edits and lightweight script checks do not require a
local AOSP build or WSL. Track contributors follow the
[handoff rules](docs/handoff/track-rules.md).

## License

[Apache License 2.0](LICENSE), as is AOSP; see
[decision 0001](docs/decisions/0001-license.md). Bundled third-party components
carry their own licenses and notices.
