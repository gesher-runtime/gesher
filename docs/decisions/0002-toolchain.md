# 0002 — Toolchain: llvm-mingw (clang, UCRT)

**Status:** accepted, 2026-10-19

## Context
AOSP is built with clang and assumes a POSIX-like environment (pthreads,
`unistd.h`, GNU-style flags). The options for a Windows target were MSVC,
clang-cl (MSVC ABI), MinGW-GCC, and llvm-mingw (clang + MinGW ABI).

## Decision
Use [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), UCRT variant,
pinned by release tag in CI.

## Why
- Same compiler family as AOSP, so ART's clang-specific code compiles as is.
- MinGW headers and winpthreads cover much of the POSIX surface.
- Cross-compiles from Linux, so CI can build on cheap Linux runners and only
  run tests on Windows runners.
- Targets x86-64 and arm64 Windows from one toolchain.

## Consequences
The toolchain version is part of the build: bumping it is a deliberate change
to `LLVM_MINGW_TAG` in CI.
