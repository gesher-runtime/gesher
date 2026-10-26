# Handoff: closing the known gaps (wave after PR #3)

Everything up to "Phase 3+" is on `main` (PR #3, merge 86aa4c7): ART with
nterp/JIT/AOT and a boot image, the framework, Gesher's own system server and
app processes, Win32 app windows, input, fonts, the NDK (ELF loader, bionic
shim, NativeActivity), the installer and an e2e job with pinned F-Droid apps.

The project owner's order after that: **close every known gap, and don't stop
until none are left.** The gaps run as parallel tracks, each on
`wip/<track>` cut from main 86aa4c7, each in its own worktree
`C:\work\gesher-<track>`. Every track agent follows
[track-rules.md](track-rules.md); each track's original brief is in
[tracks/](tracks/).

## Integration branch

`phase4/integration` (cut from main, worktree `C:\work\gesher-int`). Merge
only **CI-verified SHAs** (both test runners, package and e2e green), never a
branch tip that has commits CI hasn't run. A docs-only commit on top of a
verified SHA is fine.

Merged so far:
- popups: 9edfef6 (code verified at 6ce1d20, ci 40129561197). ADR 0040.
- rtl: 3aae3be (verified at 1c9b6b2, ci 40133992911). ADR 0044.
- Integration ci run 40135350639 on 6cea4ca (popups + rtl): all green.
- exec: merged (verified at 059b267, ci 40137662101; Puzzles passes e2e).
  ADR 0035.
- devsvc: merged (verified at a02e84a, ci 40137762701 attempt 2; attempt 1's
  windows-2022 failure was a one-off rtlapp-under-ar-EG flake, passed on
  rerun: watch for it). ADR 0038.
- webview: merged (verified at 9c610d6, ci 40139785905; Notes passes e2e).
  ADR 0041.
- stack: merged (verified at 29c5255, ci 40141531509, which already
  contained every merge above, so the integration tree after this merge is
  the CI-verified one: 296 PASS lines per runner, up from 263 at 40135350639;
  the "408" in older briefs counted differently). ADR 0036.
- devices: merged (verified at f62f1aa, ci 40142590418; Camera2 verified
  against a test Media Foundation source, no real camera on the runners).
  ADR 0043. Integration ci 40143864548 on 6111f0d (all seven merged
  tracks together): all green.
- 2026-10-25: every agent stopped on the org's API spend limit; all were
  resumed with SendMessage after it reset (their work was saved).
- locale: merged (verified at 0946451, ci 40191538466, which already
  contained 26aa8eb, so the integration tree after this merge is the
  CI-verified one). ADR 0049. Other tracks adding services must keep
  `locale` in 50-services.ps1's expected list. Leftovers: no activity
  recreate in single process, LocaleConfig overrides not persisted, no
  ACTION_APPLICATION_LOCALE_CHANGED, display-language list changes untested
  (need a sign-in).
- net: merged as ac38344 (verified at 05aaeb5, ci 40191501348; its package
  job failed once on "the .apk open command did not take lifecycle.apk to
  onResume" and passed on rerun: an unexplained flake, watch for it).
  ADR 0037. Integration ci 40194107633 on ac38344 (locale + net).
- media stage 1: merged as a13989c (verified at 32cbb82, ci 40193603079;
  Signal Generator passes e2e on an 800x400 display: e2e apps.json has a new
  optional per-app `"display": "WxH"`). ADR 0042. Integration ci
  40195168384 on a13989c (locale + net + media): green on attempt 2;
  attempt 1 failed only media's sound-output check on windows-2022 (the
  last tones split by a gap, a timing flake the media agent is fixing). Stage 2 (video) handed to the same agent.
  Leftover for a later track: Gesher has no screen orientation (landscape
  activities on a portrait display).
- lifecycle: merged as 8bfb83f (verified at aba87e2, ci 40197536380, which
  contains bf91649, so the integration tree after this merge is the
  CI-verified one). ADR 0048. locale and lifecycle now share one
  configuration path in ActivityTaskManagerService (the package's locale
  config plus the task's size override; override configs carry a growing
  seq). Resizing relaunches activities without size configChanges.
- posix: merged (verified at e76dde3, ci 40211480836, which contains
  2d88915, so the integration tree after this merge is the CI-verified one;
  241 PASS lines per runner). ADR 0045. Patches 0450/0451 change the AOSP
  cache key. Leftovers: `stat` shows no execute bit while `access(X_OK)`
  holds; no symlink resolution in DexFileLoader; ScopedFlock unsupported;
  NTFS reports writes late (IN_MODIFY often only at close).
- webview2: merged as ae3471d (verified at 55ab46a, ci 40216380541). ADR
  0050. Integration ci 40218162937 on ae3471d (posix + webview2): all
  green.
  Leftovers: on WebView2 runtime 131 (windows-2022) a click on an app view
  inside a cut-out waits until the app window's thread is woken (its input
  queue is attached to WebView2's): compat/hwui/window's thread should wake
  itself on input; no WebViewCompat (androidx boundary interfaces); web
  content not in Android's accessibility tree; geolocation position not
  verified on the runners.
- ime: merged (verified at 6b88293, ci 40218916953, which contains baf27c9,
  so the integration tree after this merge is the CI-verified one). ADR
  0039. The windows-2022 hang was Windows' own: after a Japanese
  composition, switching away from the IME waits 180 s on ImeBroker.exe's
  call into Windows.CloudStore, which never answers on Server 2022; 35-ime.ps1
  ends ImeBroker.exe there (build < 26100). The test job now has
  `timeout-minutes: 60`. compat/input/thread_watch.cc reports window-thread
  hangs (stacks, wait chains). Leftovers: Ctrl+C/V/X use Android's clipboard,
  not Windows'; no TSF reconversion; the touch keyboard gives no IME insets.
- hwsvc2: merged as cf41c06 (verified at 85f49eb, ci 40235213023; head
  cf9cf43 adds docs only). ADR 0051. Services.java conflict: ime and hwsvc2
  each removed an inner stand-in (InputMethodManager, ContentService); both
  went. Toast clicks after the device stopped (COM activator), vibrator_manager
  without a vibrator, real content observers and content URI job triggers,
  job network constraints, sleep/resume as SCREEN_OFF/ON, time zone changes,
  legacy android.hardware.Camera, Gesher Camera for ACTION_IMAGE_CAPTURE.
  Leftovers (graphics): camera preview into SurfaceView/TextureView; Bitmap
  parcels across processes (Gesher Camera's thumbnail); no YuvImage natives.
  Integration ci 40239177837 on cf41c06: all green.
- arm64 (Phase 7): merged (verified at 8d7bede, ci 40240210807 attempt 2,
  which contains d08b439, so the integration tree after this merge is the
  CI-verified one; attempt 1 failed only net's external https fetch to
  example.com on windows-2025). ADR 0047 (with a "Revision" section: the
  successor fixed STXR monitor callbacks, guest faults as crash reports, the
  DEP redirect, xmm6-15 in the System V thunk, stack args, TLS/TLSDESC, JNI
  proxies). arm64 executables via gesher-exec and ProcessBuilder; fastmem;
  NativeActivity arm64-only APK. Overhead x2.1 (compiled kernel) / x5.0
  (register loop). Leftovers: TIC-80 (e2e knownFailure) needs SurfaceView
  surfaces (graphics); no guest JIT code, guest signal handlers, LSE, HFAs
  by value. The arm64 shim fallbacks (sem_*, wchar, libm, dlopen of
  platform libs) should move into compat/bionic for x86_64 too.
- media stage 2 (video): merged (verified at 6aff13d, ci 40258833980, which
  contains 9e449b0; only handoff docs differ, so the integration tree after
  this merge is the CI-verified one). ADR 0052. MediaExtractor, MediaCodec
  and MediaCodecList over Media Foundation (H.264; decoders only),
  MediaPlayer video with A/V sync, VideoView, MediaMetadataRetriever frames,
  output into TextureView/SurfaceView (compat/media/surface plus
  frameworks/base patch 0300: the surfaces track decides whether to adopt
  or replace it). The windows-2022 sound gap is fixed (feeder threads wait
  on Track::WaitForRoom). Leftovers: VP8/VP9/AV1/H.265 software decoders and
  the Matroska extractor on wip/media-soft (not verified yet); no DXVA,
  encoders, MediaMuxer, DRM, ImageReader output, GLSurfaceView,
  TextureView.lockCanvas; no F-Droid video app in e2e.
- 2026-10-26 early morning: the spend limit stopped all seven agents
  (media, windows3, aot2, exec2, surfaces, native2, web3); each was resumed
  from its saved state after the reset.
- 2026-10-25 afternoon: the spend limit stopped the agents again; resumed
  after the reset. The arm64 agent kept falling back to claude-opus-4-8, so
  it was replaced by an Opus successor (model given explicitly) that first
  reviews its predecessor's work.
- lifecycle (earlier note): verified at 404cd11 (ci 40191526766) but not merged: it and
  locale each added a configuration-change mechanism to
  ActivityTaskManagerService; the lifecycle agent is unifying them on its
  branch.
- New wave-2 track: `webview2` (WebView leftovers, ADR 0050), on
  wip/webview2 in the C:\work\gesher-webview worktree.

## Stopped at handover (2026-10-26)

### Lead-engineer continuation (2026-10-26)

Current owner authorization (2026-10-26, explicit chat instruction): the
owner preauthorized merges to main and made the final deliverable a published
**0.1.0 release with a working Windows installer**. This supersedes the
earlier requirement to ask and wait before the final main merge. All gap
closure, CI, PR description and installer verification gates remain in force.
The coordinator alone merges integration/main and publishes the release;
track agents keep their existing branch-only scope.

Release preparation belongs to release1, ADR 0071
([brief](tracks/release1.md)). No release/tag is published yet. Next free
ADR is 0073 after reserving web4/0072 for remaining WebView frame/port/proxy/
certificate/geometry contracts ([brief](tracks/web4.md)); earlier reservation
notes below are historical.

The handover tables below are historical. windows3 is now merged as 80cb28b:
code verified at a15ade4 (ci 40285476400), branch head 063b051 adds docs only.
All 17 windows3 checks pass on each Windows runner, both summaries say
0 test(s) failed, package install/run/uninstall cleanup passes, and e2e says
0 unexpected failures (Signal Generator on the default display). The merged
tree differs from the verified SHA only in docs; no integration rerun is
needed for this merge. Main has not been changed.

Remaining windows3/input gaps are assigned to windows4 (ADR 0061 reserved;
[brief](tracks/windows4.md)): TSF reconversion/document access, candidate
insets, real IFrameworkInputPane callback coverage, old-target adjustResize
and Application-context Display rotation. Physical keyboard coverage remains
explicitly unverified. native3 reserves ADR 0062. The remaining GL/graphics
scope is assigned to [surfaces2](tracks/surfaces2.md), ADR 0063 reserved;
the first surfaces stage is now merged and this continuation is unstarted.
media4 reserves ADR 0064 for the remaining media service/native contracts
([brief](tracks/media4.md)). aot3 reserves ADR 0065 for shared-library
class-loader contexts, cloud profiles, secondary dex and oatdump
([brief](tracks/aot3.md)). The post-aot2 audit also reserves locale2/0066,
arm64-2/0067, binder2/0068 and services3/0069 (briefs in tracks/).
net4 reserves ADR 0070 for the legacy resolver ABI, remaining socket/DNS
contracts and network details ([brief](tracks/net4.md)); its 84 libc
inventory entries move from native3 to net4. Next free ADR is now 0071.
These are open work, not completion claims.

native2 is merged as 4459570: verified at
d380a3e0190f5b418cfa3ee042691db601092e25 (ci 40296960603), head 0fc20d0
adds docs only. Every job is success/skipped; both Windows jobs end with
0 test(s) failed and 26 native2 PASS lines each (both ABIs, dalvikvm, APKs
in both modes, third-party executables). Package install/run/uninstall
cleanup passes; e2e says 0 unexpected failures (TIC-80 still a surfaces
known failure). No retry. The merged tree differs from the verified tree
only in docs, so this merge needs no integration rerun. Remaining native
and POSIX work is assigned to [native3](tracks/native3.md), ADR 0062;
complete public NDK API coverage is not claimed. Open tracks must include
this new integration code before their final verification/merge.

surfaces is merged as 4f99afd: verified at b1b59a65d30eef81bdc7208a4c88dbfdeab5e35c
(ci 40307919084, attempt 2), head 180782f adds docs only. All 28 jobs are
success/skipped, both test summaries say 0 test(s) failed, and all 32
surfaces checks pass per runner. Package install/run/input/uninstall cleanup
passes. Both e2e jobs pass now-required TIC-80 and end with 0 unexpected
failures. Attempt 1 failed only the known badssl connection timeout on 2025;
the failed job passed on rerun. The merged code is the verified tree, so
no integration rerun is needed for this merge. Remaining GL, layer, hardware,
JPEG_R and parcel-storage contracts belong to surfaces2, ADR 0063.

aot2 is merged as 4d14466: verified at
ae75766b0bcbcc04f2161cb3d8b4c8fc54510cc9 (ci 40320549410), head 20650a3
adds docs only. All 28 jobs are success/skipped (22/6), with no retry.
Both runners have all 13 aot2 PASS lines and 0 test(s) failed. Package
install/run/input/uninstall cleanup and both required TIC-80/Puzzles e2e
results pass, with 0 unexpected failures. The merged code is the verified
tree, so no integration rerun is needed. Remaining compilation contracts
belong to aot3, ADR 0065. Open tracks must include this integration code
before final verification and merge.

The baseline NDK ownership inventory is in
[native-api-coverage.json](native-api-coverage.json), with later verification
attached per entry. It records symbol presence, not complete API semantics.
Property APIs belong to exec3; Android socket/process binding, resolver and
per-network DNS APIs belong to web3, with remaining libc resolver/socket
contracts assigned to net4. The four baseline bitmap exports are
verified in native3 ci 40305973210. The software Bitmap and Minikin font
foundation is now merged as f677944, verified together in ci 40344682373
at 1e881b5, attempt 2. Attempt 1 failed only the known output-tone timing
check; the failed-job retry passed. The frozen head 8b511a8 adds docs only,
and the merged tree differs from the verified SHA only in docs, so no new
integration run is needed for this merge. Remaining NDK/POSIX scope stays
open on native3; this is not a complete API-coverage claim.

| Track | Result read / change | New verification |
|---|---|---|
| aot2 | Merged as 4d14466. All 13 track PASS lines per runner, both zero-failure summaries, actual package cleanup and both required TIC-80/Puzzles e2e results read. Remaining contracts assigned to aot3/ADR 0065. | ci 40320549410 at ae75766; head 20650a3 docs only. |
| windows3 | Merged as 80cb28b after personally checking every job, all 17 track PASS lines per runner, both zero-failure summaries, package cleanup and e2e. | ci 40285476400 at a15ade4; head 063b051 docs only. |
| native2 | Merged as 4459570 after personally reading every job, both zero-failure summaries, 26 native2 PASS lines per runner, package cleanup and e2e. Public API coverage gaps remain with native3. | ci 40296960603 at d380a3e; head 0fc20d0 docs only. |
| surfaces | Merged as 4f99afd after reading every job, both zero-failure summaries, all 32 surfaces PASS lines per runner, package cleanup and both required TIC-80 e2e results. Remaining contracts assigned to surfaces2. | ci 40307919084 at b1b59a6, attempt 2; head 180782f docs only. |
| web3 | Agent read ci 40376044459 at 939ec3: all 14 web3 checks pass each runner; 2025 has zero failures, 2022 has only the BOOT profile-vs-speed JIT failure (452/404 ms). Package/both actual e2e and bounded TLS cleanup pass. Earlier 40364697280 timed out in 2025 TLS cleanup and has no zero summary there. | Not mergeable. Root owns repeated boot startup diagnosis; no timing waiver. Remaining WebView contracts belong to web4/ADR 0072 and resolver/socket contracts to net4/ADR 0070. |
| media3 | Agent read ci 40377453079 at ad00461: both Windows suites have exactly one failure, device Camera2-to-recorder frames (20/27 versus unchanged 30+ in 2 s); package/both ten-app e2e pass. Native timing distinguishes active read/delivery from a demonstrated terminal callback/client-lock cycle. | ci 40386129131 at 6293a20 tests serial oneway callbacks, capture-handle ownership, onClosed ordering and separate native phase timings; pending. No FPS/crash causal claim. Remaining media contracts stay on media4/ADR 0064. |
| exec3 | Agent read ci 40372171824: builds/package/both e2e pass; three property fixture namespace failures each and 2022 device exits after expected steps with SIGSEGV address -1. Namespace/boot-root regressions are corrected; actual native fault cause remains open. | ci 40385411337 at 80bf394 includes bounded ffmpeg preparation and narrow pre-handler fault diagnostics. Superseded 40382268730 spent 16 minutes in unbounded media preparation without Windows results, then was cancelled. |
| native3 foundation | Merged as f677944; all four dalvikvm and four APK scoped PASS lines per runner, both zero-failure summaries, package cleanup and both required Puzzles/TIC-80 e2e logs personally read. Remaining scope continues on wip/native3. | ci 40344682373 at 1e881b5, attempt 2; frozen head 8b511a8 adds docs only. |
| native3 continuation | Process follow-up fully green in ci 40367959106 at 644e7f8: personally read all jobs, five native3/four APK PASS lines per runner, both zero-failure summaries, package cleanup and both required e2e logs. It predates the oatdump integration. New Unicode boundaries pass both ABIs/modes, but 40381742392 stops on an omitted sparse-checkout reference fixture. | ci 40386670586 at 6f6ac6f includes current integration and the reference checkout fix; full Unicode parity still pending. Locked ICU4X reference separately verified in ci 40376822140 at 7ee6b32. |
| aot3 oatdump | Merged as f5ce6f6. Personally read all 28 jobs (22 success/six skipped), all four new oatdump and 14 aot2 PASS lines per runner, both zero-failure summaries, installed-copy/input/uninstall cleanup and both required Puzzles/TIC-80 e2e logs (0 unexpected failures). No retry. | Full ci 40368334642 at 96ef687; frozen head d038b75 adds docs only. Merged code is the verified tree, so no extra integration CI is needed. |
| aot3 continuation | ci 40375696804 validates/copies matching DM and inspects selected compiled code on both runners, but has two fixture/constructor failures each. The new source corrects forced interpretation, closes StrictJarFile's failed-open descriptor leak (framework patch 0650), and learns real startup methods using ART boot/AOT profiling. | ci 40385898668 at 9831182; pending. Timing thresholds unchanged. Shared-library contexts and secondary dex remain open. |
| windows4 | Agent continues Application-context display/foreground task snapshots, real InputPane COM callbacks and TSF reconversion. First display run 40377891527 has upstream asm fetch HTTP502 and a real display cache nonce failure in both e2e Signal Generator runs; no waiver. | Subsequent COM/TSF/legacy-resize source has only own local fixtures so far; real display cache correction and combined CI remain required. |
| release1 | Exact 0.1.0 installer metadata/receipts and CI-only byte promotion prepared. Independent review reproduced/fixed mutable local receipt trust and candidate/publisher attempt drift; all 34 offline tests pass at reviewed 8073a31. | No tag/release/publication. Full branch installer/release CI waits for verified integration/web3; all final gap gates still apply. |

Watches use gh run watch with --exit-status --interval 120. Review logs are
saved outside the worktrees under C:\work\gesher-ci-review (no artifacts were
downloaded). No roadmap box was ticked for the new, unverified fixes.

All open tracks must include the latest integration runtime before final
verification. The WebView capture fix is now
merged through aot2 and was also cherry-picked into web3. Do not merge
any red or partially inspected run. The final PR/main merge and release must
wait for gap closure and full verification; the owner's later explicit
advance approval above satisfies the chat-approval gate.

The continuation briefs own aot2's shared-library class-loader contexts,
cloud profiles, secondary dex and oatdump (aot3), and media3's legacy camera
recording, persistent input surfaces and VP8/VP9 encoding (media4).
Include these in the subsequent gap audit along with every other
remaining item; green track tests alone do not close the overall goal.

Roadmap audit: the old aggregate Phase 1 NativeBN/epoll/inotify/process-model
box was still unticked despite implementations being merged. Closed using
40285476400's PASS lines on both runners (NativeBN/BigInteger, EPoll,
posix probe, WatchService, FileObserver); clarified the superseded parts of
ADR 0011 without closing its outstanding LLP64 value audit.

The owner stopped all track agents on 2026-10-26 to hand the project to a
new team. Each open track committed and pushed its work and updated its
"Status and next steps" section; continue from there. None of these tracks
is merged. Read each in-progress run's logs with `gh run view <id> --log`
or `gh api repos/gesher-runtime/gesher/actions/jobs/<job id>/logs`
(artifact downloads are blocked by the local network filter).

| Track | Branch (worktree) | Tip | Last CI read | In-progress run at stop | Status section |
|---|---|---|---|---|---|
| media3 | wip/media-soft (gesher-media-soft) | dc78223 | 40262576561: part-1 leftovers pass except MPEG-4 part 2 (csd from ESDS, fixed) and a VP8+Vorbis seek tolerance (fixed); 40285626240: stage 3 compiled, link failed (mkvwriter, fixed) | 40287172595 (first full stage 3 build) | docs/media.md "Status and next steps" |
| surfaces | wip/surfaces (gesher-devices) | 8bb7076 | 40265921060 at bbff936: 31 surfaces PASS per runner; FAIL: surfacegl single process does not end after finish(); camera preview lines unread after a threshold fix | 40285764214 at 2e67a15 (hang dump, TIC-80 at 800x400) | docs/graphics.md "Surfaces (surfaces track): status and next steps" |
| web3 | wip/web3 (gesher-webview) | 58566f8 | 40262184075: proxy/PAC, CONNECTIVITY_ACTION, local https for 56-net pass; the accessibility bridge broke other apps (fixed in 77d659a); WebViews in web3app never load | 40285810159 at 77d659a | docs/webview.md "Status and next steps (web3)" |
| exec3 | wip/exec3 (gesher-exec) | 31d348e | none (no CI run yet) | none | docs/exec.md exec3 section |
| aot2 | wip/aot2 (gesher-stack) | 8551382 | 40263271836 at 7de81ac: all green except two 61-aot2 app-image checks (app classes not in the app image) | 40285423159 at ad9d360 (`--boot-image=<dir>/boot.art:<dir>/*`) | docs/compiled-code.md "Status and next steps (aot2 track, ADR 0054)" |
| windows3 | wip/windows3 (gesher-lifecycle) | 3e313ba (code a15ade4; contains integration 7711d7d) | 40258887997 at f03607f: ALL GREEN (attempt 2; attempt 1's windows-2025 job hung 40 min in 58-devices' geocoder step): orientation, multi-resume, moveTaskToBack minimizes, IME insets (adjustResize/adjustPan), Windows clipboard, window thread wake without the WM_NULL workaround; Signal Generator's e2e display override removed | 40285476400 at a15ade4 (only a roadmap section and an integration merge since f03607f) | docs/windows3.md "Status and next steps" |
| native2 | wip/native2 (gesher-arm64) | 4ac03c4 (code b6a7d88; contains integration 8ff9219) | 40263281040: ScopedFlock and GetDexCanonicalLocation gtests, exec-bit stat, zlib/SQLite/OpenSSL/libc++ (x86_64) and zlib/libpng/libjpeg-turbo/SQLite/OpenSSL (arm64) pass; x86_64 setjmp, arm64 open flags and every dalvikvm-based native2 check failed (all fixed in b6a7d88) | 40285714522 at b6a7d88 | end of docs/native-libs.md; docs/posix.md |

Notes per track:
- media3: VERIFIED earlier is only media-soft (merged). Encoders on the
  runners: Media Foundation AAC and H.264 only. Pin Just Player's SHA-256 in
  tests/e2e/apps.json from the e2e log. docs/media-soft-video.md was folded
  into docs/media.md on this branch (its false "verified by ci 40244779791"
  claim corrected); the roadmap link to it changes when media3 merges.
- surfaces: builds on compat/media/surface and patch 0300; TIC-80 still a
  knownFailure (SDL's main thread never starts); no EGL14/GLES20 Java
  natives yet (GLSurfaceView, updateTexImage into an external texture).
- web3: accessibility bridge runs only while the screen-reader flag is set;
  threads attached to ART must detach before they exit; WPAD must not
  become a PAC proxy.
- exec3: the new /system/bin/sh (compat/elf/exec/gesher_shell.cc) has its
  front end (parser, expansions) but no executor, redirections or
  builtins; it does not link yet and exec.cmake still builds the old
  minimal shell. Everything else in its brief is not started.
- windows3: closest to merge. When 40285476400 is green on every job,
  tick its roadmap boxes with that run, set ADR 0053 to accepted, and
  merge. Merging changes behaviour other tracks see: activities in other
  tasks stay resumed (multi-resume); app processes install a proxy for
  ActivityClient's controller; single-process windows get task 1, not -1.
  Not covered: the real touch keyboard (runners have none; tests post the
  handler's message), IME candidate window as insets, legacy adjustResize
  for old target SDKs, per-task rotation only. Watch for the windows-2025
  geocoder hang in 58-devices.
- native2: ART patches 0570/0571 (renumber if they collide). Not done:
  SDL testautomation, BoringSSL tests, inotify for other processes' writes,
  fcntl record locks (always granted). Fill the symbols the "native2
  symbols" INFO lines list.
- aot2: verified: profile-guided boot image (framework oat 9.0 MB, was 72.2
  MB; all oat 16.4 MB, was 96.3 MB), startup no worse, installer 88.9 MB
  (was 112.5 MB), install-time dexopt, JIT profiles, profman merge,
  background dexopt. ART patch 0540 implements LockedFile with LockFileEx
  (native2 builds on it). Reconsider `-AllClasses` in boot-image.yml
  (+6.6 MB). Use `gh workflow run ci.yml` (`ci` alone returns 422).

Wave 3 (2026-10-25 evening), cut from phase4/integration 0b55fa2, briefs in
[tracks/](tracks/), reusing merged worktrees:
- `windows3` (C:\work\gesher-lifecycle): screen orientation, window thread
  wake under WebView2, multi-resume, moveTaskToBack minimizes, single-process
  relaunch, IME insets, Windows clipboard. ADR 0053, 51-windows3.ps1.
- `aot2` (C:\work\gesher-stack): app images, profiles, speed-profile
  framework. ADR 0054, 61-aot2.ps1.
- `exec2` (C:\work\gesher-exec): fork/exec/posix_spawn from native code,
  signals between processes, executable TLS, /system/bin programs. ADR 0055,
  43-exec2.ps1.
- ADR 0051 is hwsvc2's; 0052 is reserved for media stage 2.

Wave 4 (2026-10-26), cut from phase4/integration 8c7f163, briefs in
[tracks/](tracks/):
- `surfaces` (C:\work\gesher-devices): SurfaceView/TextureView child
  surfaces, SurfaceTexture, camera preview, TIC-80's first frame, Bitmap
  parcels, YuvImage. ADR 0056, 52-surfaces.ps1.
- `native2` (C:\work\gesher-arm64): arm64's shim fallbacks into
  compat/bionic, dlopen of platform libraries, shim gaps from NDK library
  tests, posix leftovers (exec bit, symlinks, flock, IN_MODIFY). ADR 0057,
  44-native2.ps1.
- `web3` (C:\work\gesher-webview): WebViewCompat, web accessibility,
  geolocation, system proxy and the net leftovers, a local https check.
  ADR 0058, 54-web3.ps1.
- media-soft: merged (verified at b57bb74, ci 40258938663, which contains
  6aff13d; integration differed only in docs). VP8/VP9 (libvpx), AV1
  (libdav1d), H.265 (libhevc), MatroskaExtractor; PASS lines on both
  runners. docs/media-soft-video.md. Leftovers: no libavc/MPEG-4/H.263
  software fallbacks; 10-bit output as 8-bit; WebM audio and seeking
  untested.
- exec2: merged as 88722db (verified at 466f6b1, ci 40260836601; head
  bd9bc21 adds docs only; integration ci 40264089269 because media stage 2
  and media-soft came after 466f6b1). ADR 0055. vfork-style fork+exec,
  system, popen, posix_spawn, waitpid status words, kill() between
  processes through gesher-base's mailbox hook, `gesher-exec --tool` (ls,
  cat, id, ps, getprop, setprop, logcat, kill, sleep). Leftovers (exec3):
  a fuller sh; ELF TLS in executables; destroy()/destroyForcibly and
  logcat/ps/cat/kill unverified; pm/am; persistent setprop; the on-device
  variant; static executables (ENOEXEC).
  Integration ci 40264089269 on 88722db (media stage 2, media-soft,
  exec2): all green.
- `exec3` (C:\work\gesher-exec, wip/exec3): exec2's leftovers. ADR 0060,
  45-exec3.ps1. Its first agent ran on an older model and stopped at the
  spend limit (2026-10-26 morning); an Opus successor took over its
  uncommitted shell draft. The other agents were resumed after the reset.
- `media3` (C:\work\gesher-media-soft, branch wip/media-soft, continuing
  after its merge): stage 3: audio input, encoders, MediaMuxer,
  MediaRecorder, ImageReader output, a video app in e2e, plus media-soft's
  leftovers. ADR 0059.
- The surfaces track owns compat/media/surface and frameworks/base patch
  0300 (media stage 2's TextureView/SurfaceView output) from now on.

In `framework.yml` every new test app is built and copied on its own line, so
merges only add lines.

## Where each open track stopped (2026-10-25)

All branches are pushed; all worktrees are clean. Each track doc has a
"Status and next steps" section on its branch (the status of the stack,
webview and exec tracks may be less fresh: those agents were stopped
abruptly; check `git log origin/main..` on their branch).

| Track | Branch tip | Last verified | Status doc | Where it stands |
|---|---|---|---|---|
| exec (ELF processes, Puzzles; ADR 0035) | cd27227 | earlier runs had tests green on both runners; e2e for Puzzles was being checked | `docs/exec.md` | read the latest ci run on `wip/exec`; finish Puzzles in e2e |
| stack (SOE under nterp/JIT/AOT, JNI stubs, placeholders, dual JIT cache; ADR 0036) | 916f7ec (last commit is WIP, uncommitted work saved at stop) | none known | `docs/compiled-code.md` | compiled JNI stubs, placeholder mappings, oat views, dual-mapped JIT cache committed; SOE and math-entrypoint checks in progress |
| net (BoringSSL, Conscrypt, Windows roots, ConnectivityManager; ADR 0037) | 32f5660 | compile-only net.yml green at b3cca77 (40128266078) | `docs/network.md` | ci 40133972018 at 44a2e95: build, package, e2e green; test jobs' result unread |
| devsvc (alarms, jobs, clipboard, toasts, battery, wake locks; ADR 0038) | bee6187 (code 68a913f) | none fully; 40133991534 at 30ba276 had 8/10 new checks green, both failures fixed in 68a913f | `docs/services.md` (devsvc section) | read ci 40135237081 at 68a913f; if green, that's the verified SHA |
| ime (IMM32 composition, InputMethodManager, layouts; ADR 0039) | f0bc81a | none; 40134234988 green on windows-2025, windows-2022 hangs after the Japanese IME step | `docs/input.md` (ime section) | three commits with the likely fix not yet run; already merged with phase4/integration (popups) |
| webview (WebView2; ADR 0041) | 93a384e | see `docs/webview.md` | `docs/webview.md` | already merged with phase4/integration (popups); Notes (WebView) added to e2e |
| media (WASAPI, AudioTrack, AudioManager, codecs; ADR 0042) | c729b00 | da91170, ci 40129230078 (audio engine, AudioTrack, AudioManager) | `docs/media.md` | MediaPlayer/SoundPool/codec build (882a1af, run 40135129611) failing in the codec job; F-Droid audio app SHA missing |
| devices (location, sensors, Camera2; ADR 0043) | 9c684f2 (last commit is WIP) | ci 40134370867: location and sensors on both runners | `docs/devices.md` | Camera2 over Media Foundation in progress (Java side: WindowsCamera, CameraFrames) |
| posix (paths, /proc, tzdata, inotify, process model, ART gtests; ADR 0045) | 2152bcb | none; runs 40135642983 (ci) and 40135639733 (posix.yml) at 65cccdc unread | `docs/posix.md` | read those runs first |

Coordinator session 2 (2026-10-25) read those runs: integration 40135350639
had both test jobs green (e2e still running at the time); net 40133972018:
test jobs still in progress (possibly hung); devsvc 40135237081: both test
jobs failed; media 40135129611: all green except `media / codecs`; posix
40135642983/40135639733: compile failure in `build`. A successor agent was
started for each of the nine tracks above with those findings.

New tracks of wave 2 (cut from phase4/integration, briefs in
[tracks/](tracks/)): `arm64` (Phase 7, ADR 0047, tests 47-arm64.ps1),
`lifecycle` (popups leftovers, ADR 0048, 48-lifecycle.ps1), `locale` (rtl
leftovers, ADR 0049, 49-locale.ps1). Media stage 2 (video) goes to the media
track after its stage 1 is merged; ime on windows-2022 is the ime
successor's job. The roadmap box "the framework's own system services" is
closed by decision in ADR 0046 (Gesher's own server, ADR 0019).

Leftovers reported by finished tracks (for a later wave):
- popups: the system server stops (Android would only pause) the Activity
  under a translucent or dialog-themed one; popups are opaque under SkiaGL;
  no real `ACTION_OUTSIDE`; minimizing doesn't stop the Activity.
- rtl: no per-app locales; Windows setting changes don't reach running apps;
  decimal/grouping symbols and date patterns not carried over.
- posix: a process killed by a signal exits with 128+signal; exec's `waitpid`
  must decode it the same way. `persist.sys.timezone=UTC` in build.prop is
  unused today and would override Windows' zone if ever read.
- Not yet assigned at all: Phase 7 (arm64 translation); the roadmap box
  "the framework's own system services" (by design Gesher has its own server,
  ADR 0019: decide whether to reword or close it).

Expected merge conflicts: `Services.java` registration lines (net, devsvc,
ime, media, devices), `ci.yml` jobs/`needs` and artifacts (net, media, posix,
webview), `framework.yml` test apps, `CMakeLists.txt` includes, `aosp.lock`,
`compat/framework/android_runtime.cc` rows, `libcore_native.cmake` (exec and
posix), `compat/posix/src/signal.cc` (stack and posix), `tools/package/*`,
`docs/roadmap.md`, window code (ime/webview already merged popups).

## The project owner's working principles

- Replies to the owner in **Hebrew**; code, commits and docs in English, in
  the existing style. Commits end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`, PR descriptions
  with `🤖 Generated with [Claude Code](https://claude.com/claude-code)`.
- **CI first**: never build AOSP locally; WSL only when there is no other way
  (7 GB RAM; an OOM once rebooted the machine). Disk is limited.
- **Parallel tracks** in disjoint file domains, each one subagent in its own
  worktree. One agent per whole task (not tiny milestones, not huge). A
  stopped agent's successor starts from its brief plus its status section.
- **Every agent runs on Opus**: pass the model explicitly when starting one;
  never let it inherit another model.
- **Keep going without asking** until the goal: a complete, ready installer
  with every gap closed.
- **Honest reports**: tick roadmap boxes only for what CI verified.
- **Main merges and the final 0.1.0 release are explicitly preauthorized**
  by the owner in chat on 2026-10-26. Keep all verification gates and the
  coordinator's sole merge/release responsibility; do not ask again.
- Never touch `C:\work\gesher-backup-2026-10-23`.
- A word-based network filter (NetFree) sometimes blocks text or requests:
  rephrase, or write the text to a file.
- **GitHub API rate limit**: a dozen agents polling with `gh run watch`'s
  default 3 s interval exhausted it on 2026-10-24. Always use
  `gh run watch <id> -R gesher-runtime/gesher --exit-status --interval 120`,
  and on HTTP 403 back off ~10 minutes.
