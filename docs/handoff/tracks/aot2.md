# Track brief: aot2

Compiled-code leftovers reported by the stack track. The original brief given
to the first agent of this track (2026-10-25), kept verbatim. A successor
reads it together with the "Status and next steps" section in the track's doc
on its branch.

You are the "aot2" track agent of the gesher project. Your worktree is
C:\work\gesher-stack (reused) on branch wip/aot2 (cut from phase4/integration
and pushed). Work only there.

FIRST read docs/handoff/track-rules.md and follow it exactly: it is binding.
Then read docs/compiled-code.md (its last lines name your goal), ADR 0036,
the boot-image docs and boot-image.yml, tests/windows/tracks/60-boot-image.ps1
and 70-codegen.ps1, and how com.gesher.server installs an APK (dexopt).

GOAL: Android's compilation model for apps and the framework, as on a device,
each verified in CI:
- App images: when an app is installed (or on first run), compile it with
  dex2oat as Android's dexopt does (the compiler filter Android uses for a
  first install, then `speed-profile` from the app's JIT profile in the
  background, as bg-dexopt does), producing an oat/vdex/art (app image) in
  the app's dalvik-cache/oat dir, and the app runs from it (verified by
  ART's own logging/oatdump that the app image was loaded and its classes
  were not interpreted).
- Profiles: the JIT saves profiles (ProfileSaver) for apps on the Gesher
  device, and a background compile uses them.
- Framework: compile framework.jar (and the other boot class path jars)
  with a boot image profile / `speed-profile` instead of whatever compiles
  it all today, shrinking the 75 MB framework oat; measure startup and size
  before/after (60-boot-image's median-of-3 method) and keep startup no
  worse.
- The installer ships the smaller artifacts; the package job's size goes
  down accordingly.
Tests in tests/windows/tracks/61-aot2.ps1, both runners, single process and on
the Gesher device; e2e must stay green. Your file domain: dex2oat invocation
and the boot-image build (boot-image.yml, tools for it), com.gesher.server's
dexopt code, the art patches you need (new numbers after the existing ones in
patches/art), tests/windows/tracks/61-aot2.ps1, docs/compiled-code.md, ADR
docs/decisions/0054-aot2-app-images-and-profiles.md (0054 is reserved for
you), the roadmap boxes.

Coordinator notes: CI first; NEVER build AOSP locally. Other tracks run in
parallel on the shared CI (media, arm64, hwsvc2, windows3, exec2): trigger CI
only when code is ready, cancel superseded runs. Watch with `gh run watch <id>
-R gesher-runtime/gesher --exit-status --interval 120`; on HTTP 403 back off
~10 minutes. Keep a "Status and next steps" section updated with every
meaningful push. Before your final CI run, merge origin/phase4/integration
into your branch. Final message = the report track-rules.md asks for (run
ids, PASS lines, remaining gaps, edits outside your domain, the verified SHA).
