# ANGLE: OpenGL ES and EGL over Direct3D 11 (roadmap phase 3)

Android draws with OpenGL ES through EGL: hwui's GL pipeline (SkiaGL) and
apps that use GLES directly. On Windows Gesher uses ANGLE, which implements
EGL 1.5 and OpenGL ES 2.0/3.0 (3.1 on capable hardware) over Direct3D 11.
Work happens on branch `wip/angle`; the decision record is
[0018](decisions/0018-angle-build.md).

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

(filled in from CI below)

### Not done (next steps)

- **hwui on GL**: the roadmap's "drawn via ANGLE" needs hwui's SkiaGL
  pipeline (or Skia's Ganesh GL backend under hwui) on these DLLs, an
  `EGLSurface` on the Win32 window (`eglCreateWindowSurface` with the HWND,
  or a pbuffer copied into the back buffer), and Android's `libEGL`/`libGLESv2`
  entry points that apps' NDK code and `android.opengl.*` reach. None of that
  is done here; the graphics track draws with Skia's CPU raster for now.
- Skia's GL backend is not built (graphics' Skia is CPU raster only).
- ES 3.1 depends on the D3D11 feature level (WARP reports 11_1 and gets 3.1;
  not checked by the probe).
