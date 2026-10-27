# Owner-requested session checkpoint (2026-10-27)

Work is STOPPED for a new session, at the owner's explicit request. Every
worker reported clean/pushed owned worktrees. Root's own worktrees are also
clean/pushed. No integration code merge, main merge, PR, tag or release was
performed after that request. The task goal is paused for the session switch,
not complete. Two existing GitHub runs were left active; their local gh-watch
processes were stopped without canceling the runs.

## Instructions carried forward from the owner

The new primary session is the work manager only. Delegate implementation,
Git operations, CI execution/log review and integration/release operations to
workers. Keep the primary context to short assignments, evidence reports,
decisions and user updates. Use at most three workers concurrently with the
current four-slot limit. A designated integration worker must be the only
writer of integration/main/PR/tag/release operations, under the coordinator's
direction; ordinary track workers retain their branch/worktree boundaries.

Final goal remains every known roadmap/handoff/track-status/ADR gap closed,
fully verified Windows installer, coherent main PR/merge and a working
v0.1.0 release. The owner later explicitly preauthorized the main merge and
release in chat. That supersedes the original instruction to ask again at
the end; it does not waive any source/CI/installer/release verification.
Do not ask again for approval already granted. Reply Hebrew; code/docs/commits
English. Every commit, including docs/merge, ends with:

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>

All C/C++, Java and C# compilation AND native/JVM/C# fixture execution are
CI-only, including small probes. Local source/Git/lightweight parsing only;
no WSL/AOSP build. Never access C:\work\gesher-backup-2026-10-23. Use absolute
correct-worktree paths, check directory/status before edits, no bare stash.
Watch only gh run watch ID -R gesher-runtime/gesher --exit-status --interval120
(with a space before120 in the actual command);403 backoff about10 minutes.
Never download CI artifacts locally; read job logs. CI may consume pinned
artifacts with source/jar/image identity checks. Read track-rules.md in full.

## Integration and immediately mergeable leaf

Integration worktree C:\work\gesher-int, branch phase4/integration. Its code
is the already accepted Net4 mergebc6e05bea341d1dc79920deaf3f069ad690fedc4,
exact non-doc source694a4cd6b067f1deea87015bceb2ad2160260154/full40499454478.
Later13603d54 and this checkpoint add docs only. Main/release remain unchanged.

NetResolver is ready for coordinator acceptance/merge after final independent
guards: C:\work\gesher-net4-resolver, wip/net4-resolver and immutable
wip/net4-resolver-verified=50017009bc4d94b5dc0c57103686fd0761de7bf8,
docs-only above f36317637e01fdeeebcb1cbc4777c1cc8f9cea9a. Full40519019567
attempt1 is28jobs22success/6skip. Root independently read metadata and all
five actual critical logs and reviewed the bounded production/caller source:
no additional blocker. Each Windows has18 resolver passes (16 ABI/policy/
caller contexts, independent8-row WinSDK oracle and cleanup), whole0; package
23 and both complete10-app e2e/4transport/association/shortcut0 pass.

Jobs: Windows2022=122259610612;2025=122259610623; package122259610574;
e2e2022=122260927674;2025=122260927652. Source fixes Linux/WinSDK hostent/
servent layout, deep TLS ownership/error state and four legacy lookups.
Provider TRY_AGAIN/NO_RECOVERY, heap/startup failure, reentrant/per-network/
wire/general resolver contracts remain open. Inventory is byte-identical to
bc6:145 existing proofs/82 remaining entries, with no new blanket closure.
Read tracks/net4-resolver-final.md. No merge was done before this stop.

## Existing runs to read first after resume

| Run / source | State at last required-watch observation | Next action |
| --- | --- | --- |
|40522376074 /2913d484b4f3adee64acd44c5dd7b7e4a042d6f7|Libcore build37m44 success; Windows122275166484/122275166538 still active; package122275166541 and e2e122276311510/122276311525 successful conclusions|Read final metadata and actual15 CopyFileTest groups, both summaries, installed package/e2e; Root has not reread those product logs.|
|40527788805 /5d9e24d3df1e9caa77c21dbae360a1f32988b943|NDK muxer full still active when Media stopped; Windows/e2e had started|Read all final jobs and actual8 contexts x18 semantic parts/12 FD rows/10 independent decodes per runner, both0/package/e2e. This source excludes the notice/AAudio/Surface candidate.|

Restart watches with the exact command in the owner rules. Do not dispatch
another run on an active ref: concurrency cancels its predecessor. Plain
logs/reviews are under C:\work\gesher-ci-review; keep them.

## Root-owned stopped tracks

| Worktree / branch | Saved HEAD | Evidence / next action |
| --- | --- | --- |
|C:\work\gesher-exec4-spawn /wip/exec4-spawn|1ebc4b3c3dff79fbaa32786f389b23f88171a336|Scoped40520772723/9b9f625b passes all4 jobs and both four-case ABI suites. Full40521839555/b4c350b8 RED:2022 two spawn cases;2025 those two plus3 Web3 cases. See exact new path finding below.|
|C:\work\gesher-libcore-io /wip/libcore-io|7da19ebdbbc59960b12cb035190728800f9874c5|Full405223 pending on2913. Production is three registration lines exposing the already compiled upstream UnixCopyFile.transfer;15 public/native/cancellation groups. Bounded independent peer report exists.|
|C:\work\gesher-ci-processes /wip/ci-processes|6b3f24555549440aec2c6c1e3c68a88efde4c210|Scoped40526183946/ec0f003c GREEN; Root read both actual logs,4 cases each then0. Shared RunTimed has bounded termination/output waits and no unfinished Task.Result. No full dispatched; legacy SurfaceRun/test76 still open.|

Important newly identified spawn fixture cause: full run-tests passes relative
Dir=dist. Test77 derives relative self-program dist/exec4-spawn-ABI, then
deliberately changes the child's directory. Actual full logs say:

program lookup ... errno2 ... path[dist/exec4-spawn-arm64-v8a]
cwd[D:\a\_temp\gesher-root\...\a-<Hebrew>\b-<Hebrew>]

POSIX correctly looks up the relative program after child actions. The scoped
driver used absolute Dir, explaining its success. Fix test77 to Resolve-Path
the actual self-program before NoDrive, preserving production semantics and
every assertion. This fix was NOT applied before stop. No Unicode-chdir
production fix is justified. Prior fixture corrections (DETACHED_PROCESS for
natural raw stdin-2; closure check before another open reuses fd3) are proved.
Full405218 both no-console cases pass;2022 total2 and2025 total5 fail. Root
read both Windows logs122255368046/122255368845. Package/e2e conclusions
green but actual product logs not independently reread for this run.

## Windows / Web / AOT / release stopped tracks

| Worktree / branch | HEAD | State |
| --- | --- | --- |
|C:\work\gesher-windows4 /wip/windows4|92cf470c|Frozen full40466021634/source160e25e8 accepted/merged6297; physical input/richer candidate/TSF limits open.|
|C:\work\gesher-webview /wip/web3|70561d19|Observer40531106587/5bd1732e GREEN3jobs, actual WCT/stack control and14 Web3 tests both hosts. No original hang reproduced, no cause/fix claim.|
|C:\work\gesher-aot3 /wip/aot3|87ca9099|Corrected Main loaderf437 and shared-log read have component40524933356/1120 GREEN both:21 scoped+15 graph passes each. No corrected full yet.|
|C:\work\gesher-release /wip/release1|fc3e0a8a|Version/receipt/promotion/notice preparation only; no release CI/publish/tag.|
|C:\work\gesher-net4 /wip/net4|1638c908|Frozen/accepted/mergedbc6.|
|C:\work\gesher-net4-resolver /wip/net4-resolver|50017009|Full green, frozen, pending merge as above.|

Common Web3 failure is real and recurring in Locale405160, AOT405123 and
spawn405218: A11yActivity reaches onDestroy then nativeWindow.close enter
without return/fresh app0; later proxy/PAC NetActivity launch controls fail.
Native path: decStrong -> Win32Surface destructor -> Window destructor ->
Unregister/Post WM_APP/WaitForSingleObject(INFINITE); owner STA destroys
window and CoUninitialize. Existing evidence does NOT identify which native
wait/PC causes it. Java node waits are bounded; UIA raise was already async.
Do not revive the withdrawn synchronous-UIA hypothesis or rerun unchanged
seeking green. Existing observer gathers WCT/native stacks only AFTER the
original deadline failure is recorded; its green run gives no failing stack.
Read tracks/web3-teardown.md and use the observer on a meaningful failing
source. Keep original60+60/400s/fresh-child result assertions.

AOT old full405039/c35 failed missing UnixCopyFile JNI before graph checks.
Full405123/96b then had active log sharing failure plus Web3 on2025.
Component fixes: shared log read and GesherAppDriver uses LoadedApk's actual
graph instead of overriding it with bare PCL. Matching changed jars/images
were rebuilt in CI. Actual after-resource/DLC app/image, secondary/staticSDK/
factory-loader work remains open. Read tracks/aot3-shared-diagnostic.md.
Detailed Windows worker checkpoint:
C:\work\gesher-ci-review\windows4-web-release-owner-checkpoint.md.

## Native / locale stopped tracks

| Worktree / branch | HEAD | State |
| --- | --- | --- |
|C:\work\gesher-locale2 /wip/locale2|f5a02ce5|Docs-only above coherentca4ed3cf; full405160 RED solely3 Web3 failures on2025;2022 whole0. Both21 locale,5 FPCR/5 FPSR/6 Wide, package23/both10 e2e0 pass. No union acceptance.|
|C:\work\gesher-arm64-varargs /wip/arm64-varargs|c7837ff4|Coherent source0cfcd2e0 includes13603 and immutable Locale/FPSR via52be5095; component workflow prepared, no run exists (dispatch404).|
|C:\work\gesher-arm64-env /wip/arm64-env|2a3c40e2|Source audit/implementation plan only for test85 underADR0067; no production/test source or CI.|

Varargs baseline40525678026/3b607e75 proves a valid shallow literal logger
exits139 because conversion eagerly copies496 unused stack bytes; deep
control0. Scoped correction40528739976/928a09b7 GREEN4executed/1skip: original
shallow/deep0, both ABI formats/mixed overflow/80GP/va_copy/nesting/threads/
wide/binary128-source/gzip retry, actual old/new API2/API3 rejection.
Root/Media source reviews found no remaining bounded production blocker;
fixture findings were corrected. Native2 compile40529197932/64f33ff3 GREEN.
Private list ABI3 requires new host and DLL together; never overlay the DLL
alone onto an old host. Preserve x86 and copied-list readers/NextFp128.

Next: register/trigger .github/workflows/arm64-varargs-component.yml via a
narrow branch/path push trigger (dispatch on a new workflow absent from main
returns404). It builds fresh coherent hosts/DLL, verifies exact provenance
and five jars against pinned Locale artifacts, reuses boot only if identical,
otherwise rebuilds. Execute unchanged native2 ART/single/device plus FPCR/
FPSR and JNI copied-V controls. After real component proof, give Media the
same exact prerequisite. No coherent full/JNI/APK/Surface acceptance yet.
Read tracks/arm64-varargs-scoped.md and arm64-2-environment.md.

The predecessor Native agent repeatedly hit an automatic content filter;
the fresh focused successor completed ordinary engineering/checkpoints.
Keep new worker contexts focused on repository ABI compatibility and actual
CI evidence. Old chat agent IDs need not be reused in the new session.

## Media stopped tracks

| Worktree / branch | HEAD | State |
| --- | --- | --- |
|C:\work\gesher-media4-invalidation /wip/media4-invalidation|5c578ffb|Surface+AAudio+notice candidate, atomic loggerfd327 compiled40526285481; focused80+86 component WIP saved, not dispatched/imported Native yet.|
|C:\work\gesher-media4-muxer /wip/media4-muxer|74cc4b0b|Docs-only above full405277 source5d9e; compile-only40523933946/3208 GREEN, Root bounded review complete.|
|C:\work\gesher-media4 /wip/media4|58a24583|Frozen OpenSL source d10/full40476766790, Root verified bounded leaf.|
|C:\work\gesher-media4-native /wip/media4-native|35cf9aa1|Frozen native factor e569/full40477536464, Root verified bounded leaf.|
|C:\work\gesher-media4-aaudio /wip/media4-aaudio|c54a3a9f|Frozen policy sourceafeb/full404873 attempt2 RED for default TIC blank2025; no acceptance.|
|C:\work\gesher-media-soft /wip/media-soft|c54dea4d|Saved historical/milestone docs.|
|C:\work\gesher-media /wip/media|9ea09dc1|Saved historical/milestone docs.|

Surface full405078/06 RED:2022 four test80 failures (x86 phase/captures and
both ARM);2025 two ARM. Seven controlled production invalidation cases,
package23/installed notice hash and both default10-app e2e plus separate
mandatory traced TIC scenarios passed; none waives failed tests. Root fixed
ABA invalidation-ticket risk and bounded new helper before that run. Legacy
logger body remains a required independent regression; one-write phase
logging cannot mask it. No broad Surface/TIC cause claim. Prepare actual
80+86 component after Native proof; broad full should incorporate Web3 fix.

Muxer full excludes Surface/AAudio/new notice. Package green there will NOT
establish installed-notice acceptance. Runtime needs eight contexts x18
parts/12FD/10 independently decoded outputs per runner. Eight APIs exist in
the old inventory, two API31 additions are separate. Append/HEIF/B frames/
advanced metadata/public codec off_t header and other media gaps remain open.
Root review: C:\work\gesher-ci-review\media4-muxer-3208-root-review.md.
Extractor source plan only (no implementation/reservation):
tracks/media4-extractor-source-plan.md in the muxer worktree.

## Reservations, other open scope and final release

ADR0077/test83 libcore-io;0078/test84 ci-processes; test85 full ARM environment
underADR0067;0079/test86 ARM variadic lists. Next free ADR0080/test87.
Preserve frozen FPSR75e7/full40498782924 and other refs; see README for all
historical accepted leaves. Main roadmap, every Remaining/Leftovers/Not done
item and ADR/track status still defines the aggregate goal. No percentage or
ready-for-release claim is justified. Services identity/permissions, richer
Windows input, native/POSIX/network, complete ARM/JIT/signals/LSE/HFA/env,
AOT loaders/dex, media codec/extractor/DRM/hardware and other explicit gaps
remain assigned/open. Never close them by deleting/waiving an assertion or
reclassifying an unsupported path as complete.

Release1 still needs final coherent accepted runtime/notice, release_version
0.1.0 full installer verification, fresh main CI/provenance, PR with per-track
run IDs/limits, approved main merge and verified immutable release assets.
The owner already authorizes the merge/publish when all requirements pass.

The new session must delegate a fresh status/CI audit first, then allocate
the three worker slots. Avoid repeating source research or rebuilding locally;
all saved reports and exact failed/successful CI evidence must carry forward.
