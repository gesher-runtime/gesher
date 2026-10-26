# Frozen stage: native3 bitmap and font foundation

Branch: wip/native3-foundation, worktree C:\work\gesher-native3-foundation.
This freezes the verified subset while wip/native3 continues the remaining
NDK/POSIX scope. No remaining contract is removed or called complete.

## Verification

CI 40344682373, SHA 1e881b508b9a663f6d4cdd6f43ad629b8782a8ea, attempt 2.
All 28 jobs are success/skipped (22/6). The coordinator personally read
every job conclusion and the following actual logs:
- Windows2022 job 121676117601: four native3 PASS lines (Bitmap + fonts for
  each ABI), four native2 APK PASS lines for both ABIs/modes, 0 test(s) failed.
- Windows2025 job 121676120097: the same eight scoped PASS lines and
  0 test(s) failed.
- Package 121676116514: actual installed-window/input and uninstall device,
  data, association, toast and Start-menu cleanup PASS lines.
- E2e2022 121676119564 and e2e2025 121676119065: required Puzzles/TIC-80
  PASS, association/shortcut checks and 0 unexpected failures.

Attempt 1's only failure was the known media output tone timing check on
Windows2022 (1407.2 versus 1480 Hz). The allowed failed-job rerun passes;
this attempt is recorded rather than omitted.

Verified features: six libjnigraphics software Bitmap entries with direct
locked storage, data space, callback PNG/decode and negative paths; all 18
AFontMatcher/AFont/ASystemFontIterator entries over real Minikin/shipped
fonts, mixed Latin/Hebrew runs, italic style, meaningful variable axes and
metadata ownership after iterator/matcher destruction. Framework dalvikvm
and APKs exercise both ABIs and both APK modes.

The unchanged eight-case binary128 feasibility probe is separately verified
in ci 40338754636. It is not an implementation of the remaining libm APIs.

## Shared-file changes and limits

Shared edits are additive: main CI's early bridge compile and pinned APK
transport/fixtures; native2's Java build/checks; hwui's ABitmap source/patch
0620; bionic JNI targets; Native2Activity/native2 test registrations; Windows
44/46 checks; and the initial geocoder completion/45-second bound. These are
all in the verified SHA. Later fixes for WinRT teardown and log framing stay
with their active branches and need their own full combined CI.

Hardware Bitmap/HardwareBuffer, standalone native font bootstrap, runtime
font installation/updates, remaining platform handles/exports, Unicode/ABI
parity, record locks, inotify, proc metadata and file mremap stay open in
native3/surfaces2 and the explicit owner briefs. The stage does not establish
complete NDK support. No local AOSP build, WSL or artifact download was used.

## Status and next steps

Frozen from the verified code and merged with current integration's docs
only. Confirm the final branch diff against the verified SHA contains docs
only, then merge this stage into integration with --no-ff. Main/release are
the coordinator's responsibility under the owner's advance authorization.
