# 0004 — Building AOSP code with CMake, overlays and patches

**Status:** accepted, 2026-10-20

## Context
AOSP is built with Soong, which needs most of the AOSP tree (build system,
prebuilt compilers, Go, dozens of projects; tens of GB) and only knows how to
target Windows for a few host tools. We need a small, fast build that can grow
module by module and runs comfortably on hosted CI runners.

## Decision
- **CMake + Ninja**, cross-compiling with llvm-mingw
  ([0002](0002-toolchain.md)). Each AOSP module gets a CMake target under
  `cmake/aosp/` that mirrors its `Android.bp` *Windows* configuration: same
  sources, flags and generated sources. Comments name the `Android.bp` module
  they mirror, so updates to the pinned tag can be diffed by hand.
- **Sources stay upstream.** `tools/fetch-aosp.sh` shallow-clones the projects
  in `aosp.lock` at one release tag. No AOSP code is committed here.
- **Two ways to change AOSP code**, chosen per file:
  - `overlay/<project>/<path>` — a full replacement for a file that is
    Windows-specific upstream (for example `mem_map_windows.cc`). The CMake
    target lists the overlay instead of the original.
  - `patches/<project>/NNNN-*.patch` — small edits to shared files, applied in
    order by the fetch script. Keep them minimal; each should be upstreamable.
    New code paths are gated on `defined(GESHER)` (defined for every Gesher
    build), so a patch changes nothing for other AOSP builds. Create patches
    with `tools/aosp-patch.sh begin <project>` / `end <project> <slug>`.
- Everything links statically: each tool is one self-contained `.exe`.

## Consequences
- A tag bump means: update `aosp.lock`, re-check each mirrored `Android.bp`,
  rebase the patches.
- Soong-only features (aconfig flags, APEX packaging) are not reproduced; the
  Windows configurations of the modules we build do not use them.
