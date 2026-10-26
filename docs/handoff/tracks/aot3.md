# Track brief: aot3

Compilation contracts left open by aot2 / ADR 0054. ADR 0065 is reserved.
Create branch `wip/aot3` and worktree `C:\work\gesher-aot3` from the
integration tip after aot2 merges. Follow track-rules.md; CI first, no local
AOSP builds or WSL, absolute paths for every edit.

Read docs/compiled-code.md, ADRs 0031, 0033, 0036 and 0054, the aot2 test and
boot-image workflow, com.gesher.server.Dexopt and ART's pinned class-loader
context/profile handling before implementation.

Goals, each verified on windows-2022 and windows-2025:
- Replace the hard-coded `PCL[]` context with the installed app's actual
  shared-library/class-loader context. Compile and load an app with a real
  shared Java library; assert ART accepts the oat/image and executes compiled
  methods. Cover library updates and invalidation without accepting stale code.
- Consume install-time `.dm` cloud profiles with Android's validation and
  fallback behavior. Prove the supplied profile selects real compiled app
  methods; reject mismatched/invalid metadata without corrupting installation.
- Compile secondary dex files from the owning app's data directory using
  their observed loader contexts, profiles and safe ownership rules. Exercise
  DexClassLoader execution, a later dex update and stale-artifact rejection.
- Build and package oatdump for the pinned ART and use it to independently
  inspect app images, oat code and compiler filters in CI, supplementing the
  current ART execution logs.

Domain: app dexopt/install/runtime glue, ART patches, dex2oat/profman/oatdump
CMake, package artifacts and focused tests. Add an own test file using the
next available test number after checking concurrent tracks. Keep shared
workflow/installer edits additive and list them in the final report.

Before final CI, merge current origin/phase4/integration. Read every job,
both zero-failure test summaries, the actual PASS lines claiming new work,
package install/run/uninstall cleanup and both required e2e results. Never
tick a box for unverified implementation. Remaining contracts become another
explicit brief rather than disappearing when this stage is green.

## Status and next steps

Reserved at aot2's ci 40320549410 verification milestone on 2026-10-26.
Not started; no implementation, branch/worktree or CI run yet. All four
contracts above remain open.
