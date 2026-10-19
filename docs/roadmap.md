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
- [x] `mmap` emulation for ART: anonymous maps, address hints, partial unmap,
      `mprotect` (overlay `mem_map_windows.cc`)
- [ ] `MAP_FIXED` over existing reservations (placeholder mappings,
      `VirtualAlloc2` / `MapViewOfFile3`)
- [ ] Executable file mappings (`.oat` code)
- [ ] ART's own gtests for libartbase/libdexfile running on Windows
- [ ] LLP64: audit ART for `long`-is-pointer-sized assumptions (`__LP64__` is
      not defined on Windows x64)
- [ ] libart (runtime) compiles for Windows
- [ ] `dalvikvm.exe` runs `Hello` with the interpreter
