# ANGLE: OpenGL ES and EGL over Direct3D 11 (roadmap phase 3)

Android draws with OpenGL ES through EGL: hwui's GL pipeline (SkiaGL) and
apps that use GLES directly. On Windows Gesher uses ANGLE, which implements
EGL 1.5 and OpenGL ES 2.0/3.0 (3.1 on capable hardware) over Direct3D 11.
The build was done on branch `wip/angle`, hwui's GL pipeline on it on
`wip/hwui-gl`; the decision records are [0018](decisions/0018-angle-build.md)
and [0030](decisions/0030-angle-hwui-skiagl.md).

## What is built, and how

- **Source**: upstream ANGLE (`chromium.googlesource.com/angle/angle`) at
  `ab6a82e0ed1425316da0ba9a0b1c715a58a0b725`, the commit AOSP's
  `external/angle` was rolled to at android-16.0.0_r4 (its log: "Roll ANGLE
  from 238eaf1dd4d3 to ab6a82e0ed14", merged as 620766820606). AOSP's copy
  adds only Android build files; its Android.bp has no Windows backend.
- **Build** (`compat/angle/build-angle.ps1`, on `windows-2022`): depot_tools at
  the commit ANGLE's DEPS pins (`3accee08`), a shallow `gclient sync
  --nohooks` with `custom_deps` that leave out what the two DLLs never use
  (dEQP/VK-GL-CTS, SwiftShader, Dawn, LLVM, OpenCL, Vulkan layers and tools,
  catapult, Rust crates, image libraries), then only the hooks the build needs:
  Chromium's clang (`tools/clang/scripts/update.py`), `LASTCHANGE`, and
  Chromium's `rc.exe` (downloaded by the SHA-1 in
  `build/toolchain/win/rc/win/rc.exe.sha1` and checked). The runner's Visual
  Studio 2022 and Windows SDK are used (`DEPOT_TOOLS_WIN_TOOLCHAIN=0`).
  `gn gen` with `compat/angle/args.gn` (release, x64, Direct3D 11 only, no
  Vulkan/GL/D3D9/WebGPU/null backends, no tests), then
  `ninja libEGL libGLESv2`.
- **Output** (artifact `angle-sdk`): `bin/libEGL.dll`, `bin/libGLESv2.dll`,
  `lib/libEGL.dll.lib`, `lib/libGLESv2.dll.lib` (import libraries),
  `include/{EGL,GLES,GLES2,GLES3,KHR}`, `NOTICES.txt` and `ANGLE_COMMIT.txt`.
  The DLLs link the CRT and Chromium's libc++ statically and need only
  Windows' `d3d11.dll`, `dxgi.dll` and `d3dcompiler_47.dll`.
- **Cache**: `angle.yml` caches `angle-sdk` under
  `angle-sdk-<commit>-<hash of args.gn and build-angle.ps1>`; a hit skips the
  checkout and build entirely (the cache entry is a few MB).
- **Probe** (`probes/angle/angle_probe.c`): built on Linux with Gesher's
  llvm-mingw (`-Wall -Wextra -Werror`), linked against the import libraries.
  It opens an EGL display with `EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE` on the
  hardware adapter, falling back to WARP (`--warp` forces WARP), chooses an
  RGBA8 ES 3 pbuffer config, creates a 64x64 pbuffer and an ES 3.0 context,
  compiles and links GLSL ES 3.00 shaders, draws a colored quad and a
  textured quad (a 2x2 `GL_RGBA8` texture, nearest filtering) from a vertex
  array, reads the pixels back with `glReadPixels` and checks every pixel;
  "all passed (D3D11 hardware|WARP)" when everything holds.
- **CI**: `.github/workflows/angle.yml` (push to `wip/angle` on its inputs,
  workflow_dispatch, workflow_call): job `build` (Windows, cached), job
  `probe-build` (Linux: llvm-mingw, uploads artifact `angle` = the two DLLs,
  `ANGLE-NOTICES.txt` and `angle_probe.exe`), and, when the workflow runs on
  its own, job `probe` on windows-2022 and windows-2025. ci.yml runs it as job
  `angle`; `test` and `package` need it and download artifact `angle` into
  `dist`; `tests/windows/tracks/31-angle.ps1` runs the probe twice (default
  device, then `--warp`).
- **Installer**: `tools/package/gesher.iss` installs `libEGL.dll` and
  `libGLESv2.dll` next to `dalvikvm.exe` and `ANGLE-NOTICES.txt` into
  `licenses\`; the package job checks the three files are installed.

## hwui's SkiaGL pipeline on ANGLE

The decision record is [0030](decisions/0030-angle-hwui-skiagl.md).

- **Skia's Ganesh GL backend** (`cmake/aosp/skia.cmake`): Ganesh's sources
  from libskia's `android` variant (`src/gpu`, `src/text/gpu`,
  `src/sksl/codegen`; no Vulkan, AHardwareBuffer or Skia's EGL factory),
  `SK_GANESH` and `SK_GL` in the generated SkUserConfig.h.
  `GrGLMakeNativeInterface` (`compat/hwui/gl/GrGLMakeNativeInterface_angle.cc`)
  assembles the OpenGL ES interface from `libGLESv2.dll` next to the
  executable (else ANGLE's `eglGetProcAddress`). `probes/graphics/skia_gl_probe.cc`
  draws shapes, text and a bitmap with Ganesh on a pbuffer and compares them
  with Skia's CPU raster backend.
- **hwui's GL pipeline** (`cmake/aosp/hwui.cmake`): the device's
  `SkiaOpenGLPipeline`, `SkiaGpuPipeline`, `EglManager`, `GLUtils`,
  `SkiaProfileRenderer` and pipeline headers in the host hwui;
  `compat/hwui/gl/RenderThread.cpp` (the host's RenderThread plus the
  device's GL context: an EglManager, a GrDirectContext over ANGLE).
- **EGL/GLES for hwui** (`compat/hwui/gl/angle_gl.cc`): the functions hwui
  calls, defined in dalvikvm and resolved in ANGLE on first use; displays,
  configs and window surfaces through `compat/bionic/egl_gles.cc`
  (`gesher/egl_angle.h`), as for an app's .so: Android's default display is
  ANGLE's Direct3D 11 device (hardware, else WARP; `GESHER_EGL_DEVICE=warp`),
  the window surface is ANGLE's swap chain on the Gesher window's HWND, and
  the window's own `WM_PAINT` is off while it exists. hwui is compiled with
  Android's EGL native types (`EGLNativeWindowType` is `ANativeWindow*`).
  `eglBeginFrame` does nothing; `EGL_KHR_swap_buffers_with_damage` (which
  EglManager requires) falls back to `eglSwapBuffers` when ANGLE lacks it.
- **Choosing it**: the CPU pipeline stays the default.
  `GESHER_HWUI_RENDERER=skiagl`, or the system property
  `debug.hwui.renderer=skiagl` (e.g. in `$ANDROID_ROOT/build.prop`), asks for
  SkiaGL; it is used if ANGLE's display initializes, an RGBA8 ES context on a
  pbuffer becomes current and Skia gets a GrGLInterface. Otherwise stderr
  says `gesher hwui: ... OpenGL ES over ANGLE is not usable (...); using the
  CPU pipeline (SkiaCpu)` and the CPU pipeline renders
  (`compat/hwui/gl/pipeline_select.cc`, called from `register_gesher_graphics`).
  With SkiaGL stderr says `gesher hwui: SkiaGL pipeline (...): ANGLE <GL_RENDERER>, ...`.
- **Reading it back**: `com.gesher.app.graphics.GraphicsDebug` (gesher-app):
  `renderer()`, `rendererDetail()`, `captureComposed(hwnd)` (PrintWindow
  with `PW_RENDERFULLCONTENT`: what DWM composed, the swap chain included),
  `setGlReadback(true)` / `GESHER_HWUI_GL_READBACK=1` and `lastGlFrame(hwnd)`
  (glReadPixels of each window frame just before `eglSwapBuffers`).
  `NativeWindow.capture()` and `readBackBuffer()` show the CPU back buffer,
  which GL frames do not update.

## Licensing

ANGLE is under a BSD 3-clause license. The DLLs also contain zlib (zlib
license), abseil (Apache 2.0), Chromium's libc++/libc++abi (Apache 2.0 with
LLVM exception) and ANGLE's own `src/**/third_party` code (xxhash and others,
BSD/MIT-style). `build-angle.ps1` gathers every one of those license files
into `NOTICES.txt`, shipped as `ANGLE-NOTICES.txt` and installed as
`licenses\ANGLE-NOTICES.txt`, which satisfies the binary-redistribution
notice requirements. All of these are compatible with Gesher's Apache 2.0.

## Status

### Done and CI-verified

- **ANGLE builds from source in CI**: angle run 39987335568 (first build,
  cache miss): `gclient sync` about 2 minutes, `ninja libEGL libGLESv2` 565
  edges in 7 minutes on windows-2022, the whole job 10 minutes;
  `libGLESv2.dll` 5.3 MB, `libEGL.dll` 0.5 MB. angle run 39988938192 rebuilt
  it after a `build-angle.ps1` change (NOTICES.txt filter).
- **The probe passes on windows-2022 and windows-2025**, on both devices
  (angle runs 39987335568 and 39988938192, job `probe`): `GL_RENDERER` is
  `ANGLE (Microsoft, Microsoft Basic Render Driver (0x0000008C) Direct3D11
  vs_5_0 ps_5_0, D3D11-10.0.20348.5756)` (26100 on windows-2025),
  `GL_VERSION` `OpenGL ES 3.0.0 (ANGLE 2.1.1 git hash: ab6a82e0ed14)`, EGL
  1.5; "2048 yellow, 2048 textured, 0 other pixels", "all passed (D3D11
  hardware)" and "all passed (D3D11 WARP)". The runners have no GPU: their
  "hardware" adapter is the Microsoft Basic Render Driver, itself WARP.
- **In ci.yml** (ci run 39990699626 on wip/angle, all green; job `angle` hit
  the cache, 19 s): `PASS  angle: EGL on D3D11, GLES 3 shaders, draw,
  glReadPixels` and `PASS  angle: the same on WARP` on windows-2022 and
  windows-2025, every earlier check still passing; the package job installed
  `libEGL.dll`, `libGLESv2.dll` and `licenses\ANGLE-NOTICES.txt` (`PASS
  installer: installs, runs Hello and gesher-app, uninstalls`).

- **Skia's Ganesh GL over ANGLE** (ci run 40045622565 on 742d0d3 and ci run
  40047748570 on 3c38798, both runners): `PASS  angle: Skia Ganesh GL over
  ANGLE (WARP), the same pixels as the CPU raster backend`.
- **hwui's SkiaGL pipeline into a Win32 window** (same runs, both runners;
  `tests/dex-graphics/gesher/HardwareRendererGlTest.java`,
  `tests/windows/tracks/31-angle.ps1`): a RenderNode tree (a white and
  magenta root, a cyan child node through `drawRenderNode`, text in a font
  file's Typeface, a 16x16 bitmap scaled 2x) rendered by HardwareRenderer
  with the CPU pipeline, which saves its frame, then with SkiaGL in another
  process; the GL frame read back with glReadPixels and from the window
  (PrintWindow, `PW_RENDERFULLCONTENT`) equals the CPU frame: 0 pixels
  differ by more than 2 outside the text, and the text has the same 473 dark
  pixels. `GL_RENDERER` `ANGLE (Microsoft, Microsoft Basic Render Driver
  (0x0000008C) Direct3D11 vs_5_0 ps_5_0, D3D11-10.0.20348.5756)` (26100 on
  windows-2025), `OpenGL ES 3.0.0 (ANGLE 2.1.1 git hash: ab6a82e0ed14)`, EGL
  1.5 (EglManager asks for ES 2; ANGLE gives 3.0).
  - `PASS  dalvikvm: HardwareRenderer: the CPU pipeline's frame (child node, text, bitmap) for the SkiaGL comparison`
  - `PASS  dalvikvm: HardwareRenderer: SkiaGL over ANGLE (WARP) into the Win32 window, the same frame as the CPU pipeline`
    (`GESHER_HWUI_RENDERER=skiagl`, `GESHER_EGL_DEVICE=warp`)
  - `PASS  dalvikvm: HardwareRenderer: SkiaGL chosen by debug.hwui.renderer, on ANGLE's default device, the same frame`
    (`debug.hwui.renderer=skiagl` in `$ANDROID_ROOT/build.prop`)
  - `PASS  dalvikvm: HardwareRenderer: skiagl without ANGLE falls back to the CPU pipeline, the same frame`
    (dalvikvm without the two DLLs next to it)
  - every earlier check still passes (80 top-level PASS lines per runner,
    75 before), the installer too.

### Not done (next steps)

- **An Activity's UI on GL**: the window path for an Activity (the graphics
  track's IWindowSession, `wip/graphics`) presents ThreadedRenderer's frames
  with the CPU pipeline. With SkiaGL chosen, ViewRootImpl's Surface is a
  Win32Surface too, so EglManager's window surface should work unchanged:
  to verify once that path is merged (run gesher-app with
  `GESHER_HWUI_RENDERER=skiagl` and read the window back with
  `GraphicsDebug.captureComposed`).
- Not on GL: TextureView layers (DeferredLayerUpdater stands in and aborts),
  hardware bitmaps (`Bitmap.Config.HARDWARE`), HardwareBufferRenderer,
  WebView functors, the persistent shader cache and the device's GPU memory
  budget (Ganesh's default cache limit). Vulkan is not built.
- `android.opengl.*` (GLES20 and friends, GLSurfaceView) for Java apps: its
  natives (core/jni/android_opengl_*.cpp) are not built.
- The runners report OpenGL ES 3.0 on both devices; ES 3.1 and hwui on a
  real GPU through this build are unverified.
- `gclient sync` still fetches `buildtools/reclient` (~470 MB, a CIPD
  package that `custom_deps` does not drop); harmless, only on a cache miss.
