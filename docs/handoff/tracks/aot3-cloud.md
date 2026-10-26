# aot3: verified install-time cloud-profile stage

Frozen branch: wip/aot3-cloud. Verified source:
05ca072cad7a23d83403a0c039d47d4fec0c07b5, full ci 40408733247.
The coordinator personally read all 28 jobs (22 success/six skipped), both
zero-failure Windows summaries, all 16 aot3 PASS lines per runner, actual
package installation/input/uninstall and both nine-app e2e logs, including
required Puzzles/TIC-80 and 0 unexpected failures. No rerun was requested.

## What is verified

The installer validates paired metadata with the pinned DexMetadataHelper
before changing an installed package, preserves its bytes as base.dm, and
reprocesses changed metadata even for identical APK bytes. The exact baseline
helper checks the ZIP; its stale comment does not add manifest.json validation.

The real profman copy/update operation accepts only a complete dex checksum/
type/method match before atomically committing the reference profile. Matching
metadata produces selected onCreate oat code, an app image and compiled app
execution on the first launch. Missing primary.prof, corrupt profile data and
checksum/type/method mismatch fall back to verify while preserving installation
and successful APK execution. Malformed ZIP updates preserve the previous APK,
metadata and runnable app. Uninstall removes metadata and all staged/current/
reference profiles, including an interrupted cloud import.

Framework patch 0650 closes StrictJarFile's owned descriptor when native
opening fails before construction completes. The real malformed archive can
be rewritten exclusively afterward; the source is explicitly included in the
sparse AOSP fetch. Compilation and the regression pass in this full run.

The startup-profile collector now uses supported pinned ART options and the
execution-log flag required by its saved-at-exit assertion. Both full-suite
profiles are nonempty (18660/18707 bytes); profman reports actual framework
startup methods. This collects evidence without changing the boot image or
the original size/performance criteria. Both original boot comparisons pass:
2022 interpreter 561/595 ms, JIT 420/441 ms; 2025 interpreter 640/670 ms,
JIT 491/503 ms (profile/speed, medians of five).

## Installer and end-to-end verification

Package job 121876842832 reads the installed oatdump/framework image, loads
the installed fonts, installs/runs/compiles lifecycle.apk, exercises shortcuts,
the APK association and a real uiapp window/input/resize, then verifies device,
data, association, toast and Start menu cleanup on uninstall.

Windows test jobs are 121876842694 (2022) and 121876842763 (2025).
E2e jobs 121877989477/121877989195 run all nine pinned apps, all four APK
transport checks and association/shortcut launches, with zero unexpected
failures. This is a CI installer smoke result, not a published 0.1.0 release.

## Shared files and limits

Changes outside install/dexopt glue are framework patch 0650 and aosp.lock's
source path, the focused tracks 63/64, portable bounded TLS fixture ownership
(TlsServer.cs/56-net.ps1) and media.yml's bounded canonical-mirror preparation.
No test assets, tone/frame criteria or boot timing thresholds are relaxed.

Shared-library class-loader contexts (still PCL[]) and secondary-dex/profile/
invalidation contracts remain open on wip/aot3/ADR 0065. The separately prepared
startup supplement, fast-probe workflow and compiled-code checks are excluded
from this frozen source and need their own full verification. Repeated earlier
boot performance failures remain owned there; this run alone is not a root-
cause or stability claim. No local AOSP build ran. All new native compilation
and fixture execution now goes to CI under the owner's local-load follow-up.
