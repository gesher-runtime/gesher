# PR73 isolated runtime observations

This branch is a diagnostic descendant of the exact failed PR73 source
`de1e43c391275137c7b7b25dc170892684d55b5e`, tree
`15a15ae18c209f7f2ef652647efb6f2e4e7f3c91`. No product source, test, workflow
gate, assertion, or deadline is replaced. Do not merge these diagnostic files
or this diagnostic commit into a product branch.

The Windows matrix records original failures rather than declaring them fixed:

* The exact historical and freshly compiled original stdio fixtures keep all
  175 checks. An additional observation-only build records the original wide
  flush return and, if its original short-circuit condition reads, actual bytes.
* The original native process fixture keeps all 33 checks and its 10-second
  child wait. An additional build records actual DWORD status and ordinary
  UCRT child standard handles without replacing the original decision.
* The original pinned Puzzles APK provides its unmodified generator ELF. The
  exact historical `gesher-exec.exe` runs directly and through actual
  `SpawnWithFds` with the same three-pipe/OFD admission as `ForkAndExec`. Raw
  child stdout/stderr are saved. This narrower transport probe is not an ART,
  installed-app, or newly composed runtime qualification.
* The unchanged historical libbinder probe runs with file output and then real
  redirected pipes. The pipe observer samples only its own root's native
  stacks and still rejects a root alive at the original 120-second deadline.
  The outer unchanged Win32 job owner proves bounded descendant cleanup.
  Only owned thread IDs, stack PCs/module offsets, handle state and bounded
  test outputs are published; no environment dump or process-memory archive
  is collected. Root liveness and output draining remain distinct observations.

The old Windows artifact is fixed by source, run 44672812748 attempt 1,
producer 136084285734, artifact 14603016878, API metadata and raw ZIP digest.
It is explicitly a historical failed control, never a binary for new main.
Fresh component binaries carry exact diagnostic source, compiler archive,
pinned AOSP inputs, producer run/attempt and output hashes. The consumer uses
the existing current-main-identical receipt helpers and independently verifies
the source and binary manifest.

Actual main advanced to `909f04da6b5bb69bda9de450421ab10073b69b12` through PR181.
That four-file guest pthread change does not modify these POSIX component
sources. It does affect the eventual ELF runtime composition and must be
included in later product integration/Windows qualification. This failed-source
diagnostic deliberately does not claim to test that new composition.
