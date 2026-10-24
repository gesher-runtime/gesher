# Handoff: parallel tracks after Phase 2

Phases 0–2 are on `main` (PR #1, PR #2): `dalvikvm.exe` runs on Windows,
`framework.jar` is on the boot class path, and gesher-app takes an APK's
Activity to `onResume` headless, tested in CI and through the installer.

The work after that runs as parallel tracks, each on its own branch cut from
`phase3/base`. `phase3/base` adds two things to `main`: the
`tests/windows/tracks/*.ps1` hook in `run-tests.ps1`, and the Phase 3, 4 and 6
checklists in `docs/roadmap.md`. Every track agent follows
[track-rules.md](track-rules.md).

| Track | Branch | Goal (roadmap) | Track doc | Windows checks |
|---|---|---|---|---|
| Graphics and input | `wip/graphics` | Phase 3: Skia and hwui, ANGLE, a Win32 window, input, fonts | `docs/graphics*.md` | `tests/windows/tracks/30-graphics.ps1` |
| Native libraries | `wip/native-libs` | Phase 6: ELF loader, bionic on Win32, NDK `.so` from an APK | `docs/native-libs*.md` | `tests/windows/tracks/40-native-libs.ps1` |
| Binder and services | `wip/services` | Phase 4: cross-process Binder, the framework's system services, `.apk` association | `docs/services*.md` | `tests/windows/tracks/50-services.ps1` |
| Core libraries | `wip/libs-full` | Phase 2's last item: libutils, libcutils and libandroidfw in full, with AOSP's gtests | `docs/core-libs.md` | `tests/windows/tracks/20-libs.ps1` |

Each track doc has a "Status and next steps" section, written on that
track's branch: what CI verified (with run ids), what is half-done, and what
comes next.

## Where each track stopped (2026-10-23)

All four branches are pushed with clean working trees. No roadmap box of
theirs is ticked yet.

- **`wip/libs-full`** (fc5716e): every source of libutils, libcutils,
  libandroidfw, libutilscallstack and googletest compiles in CI (libs run
  39942371602). Two link errors are left: `posix_memalign` is declared in
  gesher-base but not defined, and `android::util::ExecuteBinary` is used by
  `PosixUtils_test`. Because of them, no gtest has run on Windows yet.
  libutils now uses its pthread threading under `GESHER`, so check hwui and
  binder against that change.
- **`wip/graphics`** (29e9522): Skia (CPU raster), FreeType and the codecs
  are verified on windows-2022 (skia_probe, run 39941675902; the first Phase 3
  item can be ticked once that check runs from `ci`). HarfBuzz and minikin
  compile. hwui fails with 30 compile errors from four causes, listed in
  `docs/graphics.md`. Not started: the Win32 window, input, ANGLE and fonts.
  Don't run `ci` on this branch before `ci.yml` downloads the `graphics`
  artifact, or `30-graphics.ps1` fails.
- **`wip/native-libs`** (1dabf66): ci run 39944128455 is green on both
  runners. `PASS  ELF loader: load a trivial .so and call it`. WRFSBASE runs
  on the runners, but Windows drops the `%fs` base on kernel entry, so bionic
  TLS needs another approach, most likely load-time patching of `%fs`
  accesses (ADR 0014, native-libs). The bionic shim is written but not wired
  into CMake. Not started: JNI through the loader, and libc++_shared with
  exceptions.
- **`wip/services`** (1c56b34): **does not build.** `dalvikvm.exe` fails to
  link because `ServiceManagerNative`'s registration pulls in
  `UtilsHost.cpp`; the two fixes are in `docs/services.md`. binderd (the
  `/dev/binder` broker), servicemanager and the probes compile in CI. The
  cross-process Binder tests passed only locally, with WinLibs g++. Not
  started: the framework's real system services, and the `.apk`
  association.

Expect these conflicts when merging:
- `aosp.lock`: graphics and libs-full both add `external/libpng`.
- ADR numbers: three tracks each wrote an ADR 0014; renumber them.
- `CMakeLists.txt` and `ci.yml`: `include()` lines and the new jobs'
  `needs`.
- `compat/framework/android_runtime.cc`: graphics and services both edit it.
- Unwinders: `compat/unwindstack/` (libs-full) and ART's
  `compat/art/unwindstack_stub.cc` define the same functions, so no
  executable may link both.

## The project owner's working principles

- **As much as possible in CI**, to save the local machine's memory and disk:
  builds, AOSP fetches and tests run on GitHub Actions, not locally. The
  local machine has about 20 GB of free disk, and WSL only 7 GB of RAM (an
  earlier out-of-memory once rebooted the machine). Avoid WSL altogether
  where possible, as long as that doesn't hold the work up.
- **As many steps in parallel as possible:** independent tracks on their own
  branches, merged later and verified in CI together.
- **Keep going to the end without stopping to ask:** the target is a fully
  ready installer. Don't stop before it.
- Heavy, iterative work goes to subagents with disjoint file domains. The
  coordinating session verifies, merges and commits. Don't sit idle on
  builds; run them in the background.
- **Report honestly** what CI verified and what it didn't.
- Replies to the owner are in Hebrew. Code, commits and docs are in English,
  in the existing style. Commits end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`, and PR
  descriptions with `🤖 Generated with [Claude Code](https://claude.com/claude-code)`.
- **Merging to `main` only with the owner's explicit approval.**
- A word-based network filter (NetFree) sometimes blocks text or requests.
  Work around it by rephrasing, or by writing the text to a file.

## Coordinating

- Read the track's status section on its branch, then check its latest CI
  run yourself (`gh run view <id> -R gesher-runtime/gesher --log`) before
  trusting it.
- A finished track merges into an integration branch (`phase3/integration`,
  cut from `phase3/base`). Run CI on the result, fix conflicts there
  (`ci.yml`, `CMakeLists.txt`, `docs/roadmap.md`, `app/` are the shared
  files), then open a PR to `main`.
- **Merging to `main` needs the project owner's explicit approval.**
- CI runs on `pull_request`, on push to `main`, and on demand:
  `gh workflow run ci -R gesher-runtime/gesher --ref <branch>`.
