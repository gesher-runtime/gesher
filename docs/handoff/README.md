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
