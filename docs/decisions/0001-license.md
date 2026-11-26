# 0001 — License: Apache 2.0 (Superseded)

**Status:** superseded on 2026-11-25 by [LICENSE](../../LICENSE) (Proprietary — All Rights Reserved).

> **Note (2026-11-25):** Going forward, Gesher is licensed under a proprietary
> All Rights Reserved license in [LICENSE](../../LICENSE) with no public
> redistribution permitted (with or without modifications) without prior written
> permission. Prior releases and third-party components remain under their
> respective licenses.

## Context
Gesher's core is AOSP code (ART, libcore, framework, HWUI), which is Apache
2.0. The other projects we expect to build on are permissive too: ANGLE and
Skia (BSD-3), Box64 (MIT), llvm-mingw (Apache 2.0 with LLVM exception).

## Decision
Gesher is licensed under the Apache License 2.0.

## Consequences
- Our changes can be carried in the same files as AOSP code with no license
  mixing, and could be offered upstream.
- The patent grant protects users and contributors.
- We **must not copy code** from GPL projects that solve similar problems,
  notably the Android Translation Layer (ATL) and Wine (LGPL). They may be
  studied, not copied.
