# 0003 — CI does the building

**Status:** accepted, 2026-10-19

## Context
A full AOSP checkout is around 300 GB and a full build needs a large machine.
Contributors should not need one.

## Decision
- All builds and tests run on GitHub Actions. Linux runners cross-compile;
  Windows runners execute tests.
- Only the AOSP projects Gesher ports are fetched, shallow, at a pinned
  release tag (`android-16.0.0_r4`), never the full tree.
- Build outputs are cached between runs so incremental builds stay well
  inside the 6-hour job limit.

## Consequences
- Every change is validated on real Windows (Server 2022 and 2025) before merge.
- When the ART build outgrows hosted runners (disk is the first limit), we add
  a remote build cache or larger runners; this is tracked in the roadmap.
