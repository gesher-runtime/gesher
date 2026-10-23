# 0018 — ANGLE: upstream at AOSP's commit, built with its own GN build on Windows

**Status:** accepted, 2026-10-23 (ANGLE track)

## Context
Phase 3 needs OpenGL ES and EGL on Windows: hwui's GL pipeline (SkiaGL) and
apps that draw with GLES call them, and the roadmap names ANGLE over
Direct3D 11 for it. Gesher builds everything else from AOSP sources pinned at
android-16.0.0_r4 with CMake and llvm-mingw ([0002](0002-toolchain.md),
[0004](0004-build-system.md)).

AOSP's `external/angle` at android-16.0.0_r4 is upstream ANGLE plus AOSP's
build files: its history ends in "Roll ANGLE from 238eaf1dd4d3 to
ab6a82e0ed14", merged as 620766820606, so its tree is upstream commit
`ab6a82e0ed1425316da0ba9a0b1c715a58a0b725` with Android.bp files added. Its
Android.bp builds the Vulkan backend for Android only; nothing in AOSP builds
the D3D11 backend. ANGLE's only maintained build is GN, with Chromium's
`build/`, `buildtools/`, clang and libc++ from its DEPS (gclient).

## Options considered
1. **AOSP's external/angle through Gesher's CMake** (`gesher_bp_cc` over its
   Android.bp, as Skia was done). The Android.bp has no Windows/D3D11 sources
   or defines; the D3D11 backend's source lists, HLSL translator and
   generated files would have to be rebuilt by hand from GN, then kept in
   step. It would compile with llvm-mingw, against mingw-w64's D3D11 headers,
   which ANGLE does not test.
2. **Upstream ANGLE with its own GN build on a Windows runner** at the same
   commit AOSP ships, with Chromium's clang-cl and lld-link against the
   runner's Visual Studio 2022 and Windows SDK 10.0.26100
   (`DEPOT_TOOLS_WIN_TOOLCHAIN=0`). This is how ANGLE builds for Chrome on
   Windows, so the D3D11 backend is in the configuration ANGLE tests.
3. **A prebuilt ANGLE** (from a browser or a package manager). Not from an
   official source with a pinned, reproducible build; ruled out.

## Decision
Option 2. `compat/angle/build-angle.ps1` checks out ANGLE at `ab6a82e0`
(the commit android-16.0.0_r4 ships) and depot_tools at the commit that
ANGLE's DEPS pins, runs a shallow `gclient sync --nohooks` whose
`custom_deps` drop what the two DLLs never reach (test suites, other
backends, LLVM, Rust crates), runs only the hooks the build needs (Chromium's
clang, LASTCHANGE, rc.exe, fetched by hash), and builds `libEGL` and
`libGLESv2` with `compat/angle/args.gn`: release, x64, Direct3D 11 only
(WARP is reached through the same backend), no tests.
`.github/workflows/angle.yml` caches the result by ANGLE commit and the
hash of the script and arguments, so the long build runs once per change.

EGL and GLES are C APIs with the Windows x64 calling convention, so code
built with llvm-mingw links to the DLLs through their import libraries
(`libEGL.dll.lib`, `libGLESv2.dll.lib`, read by lld in MinGW mode);
`probes/angle/angle_probe.c` is built that way. No C++ crosses the boundary,
so ANGLE's static libc++ and the MSVC-style CRT inside the DLLs do not mix
with Gesher's.

## Consequences
- The DLLs depend only on Windows' own DLLs (`d3d11.dll`, `dxgi.dll`,
  `d3dcompiler_47.dll`, which Windows 10 and 11 ship), and link the C runtime
  and libc++ statically.
- Moving ANGLE means changing `ANGLE_COMMIT` (and `DEPOT_TOOLS_COMMIT` from
  its DEPS) in angle.yml; to follow AOSP, take the commit from
  external/angle's last "Roll ANGLE" merge at the new tag.
- The installer ships `libEGL.dll`, `libGLESv2.dll` and
  `licenses\ANGLE-NOTICES.txt`: ANGLE's BSD license and the licenses of the
  code linked into the DLLs (zlib, abseil, libc++, ANGLE's own third_party).
- hwui's GL pipeline and Android's `libEGL`/`libGLESv2` loader are separate
  work: they would call these DLLs as Android's EGL/GLES implementation.
