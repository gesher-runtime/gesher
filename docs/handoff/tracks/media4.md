# Track brief: media4 — remaining media contracts

Status: active follow-up to media3, 2026-10-27. ADR **0064** belongs to
media4. The coordinator assigned C:\work\gesher-media4, branch wip/media4,
cut from integration f17d03b1 after Media3's verified merge. Read docs/handoff/track-rules.md,
docs/media.md and ADRs 0042, 0052 and 0059 first. CI only for AOSP builds;
no local AOSP builds, WSL or artifact downloads. This brief records gaps,
not completed features or authorization to use another track's worktree.

Keep the [source API boundary inventory](../../media-api-inventory.md) and
the media implementation inventory linked from [docs/media.md](../../media.md):
the [audio/codec design](../../decisions/0042-media-audio-engine-and-codecs.md),
[video/surface design](../../decisions/0052-media-video.md),
[recording design](../../decisions/0059-media3-recording-and-encoders.md) and
[Phase 8 checklist](../../roadmap.md#phase-8-checklist). Native registration
and reflection stand-ins are described in
[framework-natives.md](../../framework-natives.md); successful sample-app
coverage does not prove the remainder of those native/API contracts.

The machine-readable [native export baseline](../native-api-coverage.json)
records 313 originally unresolved media symbols assigned to media3: 276 libmediandk.so,
34 libOpenSLES.so and 3 libaaudio.so (ci 40296960603; both runners/ABIs).
media4 inherits these open exports; coordinate central per-entry ownership
updates with the coordinator, retain the recorded baseline, and add exact
subsequent verification rather than overwriting missing with a broad claim.
Full ci 40459815375 at c5962291 verifies the first slice: 21 AMediaFormat
functions and all 152 pinned data definitions. Per-entry subsequent
verification closes 143 old entries (21 functions and 122 data objects),
preserving every historical baseline/owner/observation. Thirty additional
pinned data objects are verified without extending the old export list.
Both runners/ABIs now report 133 missing libmediandk entries; 170 old media
entries remained at that format slice. Full ci 40476766790 at d10bea3a
subsequently verifies the 34 old OpenSL IID data entries, including pinned
values, indirection, copied-ID lookup and explicit capability outcomes.
Only their verification objects are added; all old fields and all 143
format proofs stay unchanged. The remaining old media entries are now 136:
133 libmediandk and three AAudio. Broader OpenSL methods remain open. Ordinary
format-key storage does not implement the media feature named by that key.

## NDK export contracts

- Supply the native libmediandk codec, format, extractor, muxer, image/reader
  and protected-media interfaces, coordinating image/reader ownership with
  the devices/surfaces tracks. Java API round trips do not implement these
  ELF exports. Reuse functional backends and test NDK lifecycle, data and
  error behavior on both ABIs/runners; export resolution alone is insufficient.
- Full ci 40476766790 verifies all 53 canonical OpenSL UUID pointer
  objects (19 corrected and 34 old missing), copied-ID matching, the typed
  query ABI, required/optional negotiation and explicit exposure in both
  JNI/APK ABIs/runners. Bare-core/no-identity denial and real APK manifest
  allowed/denied outcomes are independently checked with actual 0x0B
  results. Canonical effects/3D/device IDs retain unsupported outcomes;
  no effects engine is inferred. Standalone backend/TLS/callback proof now
  passes in separate native source e5693412/full40477536464. Configuration,
  output-mix, loss-of-control and full effects/device contracts remain open.
- Implement AAudioStream_isMMapUsed, AAudio_getMMapPolicy and
  AAudio_setMMapPolicy with correct Windows capability/policy semantics;
  verify getter/setter/result behavior rather than adding generic zeros.
  The pinned API starts policy at UNSPECIFIED (0), separately from a Windows
  effective no-MMAP capability; its setter accepts 0/1/2/3 and preserves the
  old value on invalid -898. ALWAYS with no system MMAP service fails stream
  open with NO_SERVICE (-881). No-MMAP stream identity, policy changes and
  callback lifetime must be verified; these are source-only planning facts.
- Implement AAudio error/disconnection callback delivery (currently stored
  but never invoked), device/route selection and the ignored policy setters;
  verify removal, stop/close cancellation and callback lifecycle separately
  from successful tone capture. AudioRecord's input-device setter likewise
  reports success without changing routes and needs a real route contract.

## Verified boundary

media3 implements file-fed audio input, AAC/H.264/Opus encoding, container
writers, audio/Surface/Camera2 MediaRecorder and ImageReader decoder output.
Verified mux round trips cover H.264/AAC MP4 and Opus-only WebM, not WebM
video/audio interleaving. Media3's original camera throughput and terminal
gates pass on both runners in full GREEN 40448521754 at dd06d296 (37 media3
and 12 part-1 device checks per runner; both whole suites zero failures,
package and both required ten-app e2e green). The coordinator merged Media3
as a1edcb02; this evidence closes none of Media4's API gaps.
The older 40438975894 source remains RED for five Windows-2025 net checks.
The exact verified runs and remaining failed checks are in docs/media.md. CI uses
a known WAV microphone source and GESHER_CAMERA_TEST; it does not establish
physical microphone, loopback or Windows hardware interoperability.
Legacy Binder MediaSession round trips and Just Player launch pass in
40305909645, but the optional Session2 module is absent. Null guards permit
legacy use; they do not implement that module.

## Required session contracts

- Connect active sessions' metadata, playback state and transport commands
  to Windows SystemMediaTransportControls. Verify publication, updates,
  command delivery and release/owner death on a real desktop; add a CI
  seam that verifies the same state/command adapter without a user session.
- Implement media-key dispatch with the registered receiver fallback when
  no active session handles it. Test down/up, an owner process exiting,
  ordering between sessions and the fallback receiver.
- Provide the Session2/media communication module and service, including
  token lifecycle and controller callbacks. Test a Session2 owner/controller
  round trip before replacing framework patch 0591's absent-module guard.
- Enforce the session/listener permission and user boundaries. Test allowed
  and denied callers, cross-user filtering and listener removal/death;
  current same-app tests establish none of those security boundaries.
- Deliver externally initiated local volume changes to controllers. Test
  AudioManager changes, Windows session volume events and remote-volume
  owner callbacks with accurate absolute/relative volume information.

## Capture and recorder validation still required

- Verify physical WASAPI microphone capture and loopback, device removal,
  route changes, timestamps and cancellation on hardware. Retain the known
  file source for deterministic CI and document hardware-only evidence.
- Add independent MediaRecorder lifecycle coverage for pause/resume, maximum
  duration/file-size notifications, reset/reuse and stop/error handling.
  ADR 0059 describes implementations, while the current integration checks
  cover successful recordings and getMaxAmplitude only.
- OpenSL RECORD_AUDIO allowed/denied APKs and bare-core denial now pass
  independently in full40476766790 on both ABIs/runners and single/device
  modes. Independent AAudio denial and native-child identity propagation
  remain open; the earlier Java boundary alone does not prove them.
- Exercise the software H.264 encoder explicitly with GESHER_VIDEO_ENCODERS=soft
  on both runners. Ordinary stage-3 selection tries Media Foundation first;
  ci 40330465222's 2022 log records MF I420 input rejection and actual libavc
  fallback. That fallback evidence does not replace a forced-soft check.
- Positively verify the Media Foundation H.264 encoding path on compatible
  inputs, including input-format negotiation and independently decoded
  pictures. Current CI rejects its I420 setup and selects libavc, so it does
  not prove successful MF H.264 encoding or hardware acceleration.
- Independently verify cumulative AudioTrack underrun and AAudio output
  xrun getter semantics over several starvation/recovery cycles, separating
  incidents from lost frames and preserving lifetime counts across restart.
  The media3 three-cycle counter proof covers input overruns only.

## Container validation still required

- Verify MediaMuxer's VP8/Vorbis and VP9/Opus WebM video/audio tracks and
  timestamp interleaving on both runners/modes. The source writer contains
  these paths, while media3's WebM mux proof is Opus audio only. Remux the
  existing encoded test clips to exercise the muxer independently of the
  still-absent VP8/VP9 encoders; read tracks/samples/timestamps back and
  independently decode the expected frames/colors and audio tones.
- Verify unequal track lengths, pending-sample flush at stop, ordering,
  seeking/cues and invalid-format/lifecycle/timestamp behavior. Preserve
  the distinction between implemented writer paths and tested contracts.

## Deliberate feature gaps requiring separate implementation

- Legacy MediaRecorder VIDEO_SOURCE_CAMERA/setCamera recording (Camera2
  into getSurface is the supported camera recording path).
- Persistent codec input surfaces and their detach/reuse lifecycle.
- Encoders beyond AAC, Opus and H.264: AMR, Vorbis, VP8/VP9/HEVC and any
  advertised format must be implemented before being listed as available.
- Hardware DXVA/D3D11 decode/encode, encoder B-frames and 10-bit output.
- Java MediaCrypto, MediaDrm and MediaDescrambler, encrypted extractor
  samples and secure codec paths. These native families currently use
  reflection stand-ins; the protected-media NDK exports are also missing.
  Implement explicit capability/failure behavior and supported key/session/
  buffer lifecycles with independent negative and release tests before
  claiming protected playback or hardware integration.
- Input audio effects (AEC/AGC/noise suppression), output effects/offload,
  MediaProfiles/CamcorderProfile and a real MediaStore provider.

Camera2 reprocessing, offline sessions, extensions, torch and the remaining
camera-service contracts belong to the coordinator's [services3](services3.md) track.
media4 owns recorder input/lifecycle contracts and coordinates camera-to-recorder
integration with that owner; it does not duplicate the camera-service scope.

## Completion evidence

Use own media tests with distinct PASS lines, both Windows runners and
single-process/device modes for runtime behavior. Keep e2e green and pin
new third-party apps. Merge current integration before final CI. Report the
exact SHA, job ids, PASS lines and unsupported contracts; do not tick broad
roadmap boxes based on one app launch or successful packaging. Shared files
must have minimal additive edits and be listed in the final report.

## Status and next steps

Corrected coherent Surface/AAudio full ci40507882249 is complete RED at
06d30d403481d3833f0c7d5f99db456841b72062:20 successful, six skipped and two
failed Windows jobs. Actual2022/2025 logs end4/2 test(s) failed, exclusively
new test80 marker transport and ARM ordinary-call crashes (see surfaces2).
All seven scheduling controls, format10/OpenSL16/AAudio16 scoped PASS lines
pass on both hosts. Package has23 actual PASS and the exact installed notice
hash. Both actual ten-app e2e jobs, original untraced TIC, separate mandatory
traced TIC, four transport controls and association/shortcut pass with0
unexpected failures. Source06 and the original red AAudio source remain
immutable evidence; no unchanged retry or new inventory acceptance follows
from a red full. Native owns the generic ARM va_list diagnosis/correction;
the one-write marker correction and exact CI artifact symbol inspection
remain local to this Surface branch. Later status/inspection commits do not
change the frozen runtime. Legacy post-kill wait infrastructure is assigned
to Root's separate test84. The unverified native muxer stays in its separate
branch and is not imported here.

The corrected coherent candidate is prepared separately in
C:\work\gesher-media4-invalidation / wip/media4-invalidation after merging
accepted Windows integration6297e80a. Its bounded Surface2 invalidation
correction, test80 and post-failure no-input TIC diagnostics remain pending
full acceptance;
see surfaces2.md and proposed ADR0063. All original blank/text thresholds,
AAudio/OpenSL/format/native checks and installed notice checks remain
enabled. The exact source review and first full are recorded above; runtime
repairs and a final green full remain required. Frozen format/OpenSL/native
verified refs and the separate red AAudio source remain unchanged. No AAudio
inventory acceptance follows from the red full40487303965 attempt2; its
TIC-80 cause remains unproved. No source-only muxer factoring is imported
into this candidate. Test79 remains reserved for that separate muxer work;
test80 is invalidation,81 Locale2,82 Resolver,83 libcore I/O,84 process test
infrastructure and86 ARM varargs. Further reservations are coordinator-owned.

Full40487303965 attempt2 is complete RED at the same afebf1ec source:
28 conclusions=21 success/six skipped/one failed e2e Windows2025. Both
Windows122149452806/122149452762 finish0 test failures and all16 test78
passes were read per runner. Strict log checks verify12 ABI/context cases,
60 semantic parts,192 invalid-policy preservation controls,36 real legacy
output controls,24 mandatory-MMAP -881/null results and four allowed capture
reads per runner; each ABI reports libaaudio56 checked/0 missing. No three-
entry inventory acceptance follows from a red full run. Graphics retry
122144503347 passes11m28s. Package122149452841 passes all23 actual checks
and exact installed notice SHA256 B78E6FE1973849F5F31CBF37835C907174D6A816721FD89E5EB734921F54E8DE.
E2e2022/122150311898 passes ten apps/four transports/association/shortcut,
0 unexpected. E2e2025/122150312245 passes the other nine and all transports/
association/shortcut, but TIC-80's initial capture has0/320000 non-background
and0 text pixels,1 unexpected failure. Click subsequently changes320000
pixels; close/exit is normal. Both complete app logs contain fonts.xml
ENOENT followed by successful installed-font mapping, SDL focus/native main,
EGL initialization and TIC hello; the font warning is not an established
cause. A source-backed SurfaceView pending-latch defect is being addressed
in a separate corrected worktree with cause uncertainty retained. No unchanged
TIC rerun or threshold relaxation is planned. Original blank/text failure
stays a failure even when later diagnostic captures are added.

Exact actual logs/counts are in C:\work\gesher-ci-review with prefix
media4-aaudio-40487303965-attempt2. Read-only CI40496914216 printed the
existing complete TIC app/launcher logs inside CI, without executing the app
or downloading artifacts locally; its job logs are media4-tic80-recorded-
40496914216-{2022,2025}.log. Source afeb and all accepted immutable refs stay
unchanged. Latest accepted integration6297e80a must be imported into the
corrected Surface/AAudio/native candidate before its final full CI.

Full ci **40487303965** is running at frozen source
**afebf1ec28308673bb9b0a7745547753c6123da8**, with latest accepted integration
db84749b and verified native backend35cf9aa1/e5693412 imported. It verifies
test78's actual both-ABI core/ELF/allowed-denied APK contracts alongside all
original OpenSL/format/media gates, plus the exact installed OpenSL notice.
Later commits are docs only; all three AAudio inventory entries remain
unverified until every job/both Windows whole0/package/both e2e gate passes.
Watch uses interval120. All compilation and fixture execution remain CI-only.

Attempt1 ended RED:26 job conclusions=16 success/nine skipped/one failure.
Graphics122132200985 fails before compilation while fetching AOSP, with
two actual `RPC failed; HTTP 429` messages followed by `fatal: expected
flush after ref listing` / `expected 'packfile'`; fetch xargs exits123.
Windows/package/e2e are skipped and prove nothing. Main122132200568 passes
22m47s: actual early AAudio object compile, both host links and203 assembly
boundaries/zero problems. Media122136369135 passes1m34s with both actual
ABI DSO/ELF builds and four real manifest APKs; framework122132283933
passes9m18s. Actual main/media/failing-fetch logs were read. After more
than20 minutes from the upstream429, one bounded `--failed` rerun was
requested on the same immutable source. No source change or API acceptance
follows; a repeated fetch failure will be investigated rather than retried
indefinitely. New watch session86977 uses the required interval120.

Separate AAudio policy source is prepared on wip/media4-aaudio in
C:\work\gesher-media4-aaudio, from accepted OpenSL/docs58a24583. Test78 is
reserved (next79), ADR0064 retained. It supplies the three old missing
API36 entries: process-global requested policy starts UNSPECIFIED, accepts
0/1/2/3 and preserves the previous value on invalid signed32 input; actual
shared Windows streams retain non-MMAP identity across policy changes.
ALWAYS fails new input/output opens with NO_SERVICE(-881)/null because no
Android MMAP service exists; format validation takes priority. Typed dynamic
calls use exact pinned API36 signatures with the older r27 NDK, alongside
real legacy PCM output and manifest-aware capture/read/stop/close cases.
Both-ABI libraries and genuine ELF executables, four allowed/denied APKs,
core DEX and strict test78 markers are prepared. No compilation/runtime proof or inventory change is
claimed. Standalone coverage must use the separately verified real backend,
not a policy-only library; real native full40477536464 is now green and imported.
Each ELF has the real argv0 and executable caller marker, shares every
policy/output/validation case, and independently denies input without
identity. Test78 requires16 passes per runner, including both ELF callers
and four device shutdowns. CI compiles the exact AAudio host object before
the full graph to expose header/ABI errors early. Full
builder validation, platform-policy getters, routing and error callbacks
remain open. Only PS AST/Bash/XML/YAML/source checks ran locally.

This separate AAudio candidate also applies the exact prepared OpenSL
installer-notice proposal (f9c4197c patch): the preserved Khronos notice is
installed under licenses and package CI compares its SHA256 to the source.
Neither frozen d10/e569 candidate changes. Installed-byte proof is pending
the new candidate's package job; this is no general license-completeness
claim and preserves all23 existing installer PASS gates.

Native OpenSL full ci **40477536464** is complete GREEN at
**e569341230b3970cadaeb48a235a12a4645da74a**, attempt 1 without retries.
All 28 conclusions were read:22 success/six skipped. Main122099364029
passes36m30s (actual record_permission.cc compile, both executable links,
203 assembly boundaries/zero problems); framework122099455423 passes8m18s,
media122102886520 passes1m22s with both real NDK ELF/library builds and all
four APKs. Windows **122115058999** (2022,28m54s) and **122115058759**
(2025,28m12s) each finish **0 test(s) failed** with16 actual test76 PASS.
Both ABIs now run core JNI, real standalone ELF and all four allowed/denied
single/device APK contexts, plus four device shutdowns per runner. All60
semantic rows,48 volume rows and12 actual callback traces per runner were
read: calls1/TLS valid true/caller TLS unchanged true, alongside all53
canonical UUID/data/query/negotiation contracts. Bare core and standalone
native processes correctly deny recording11/no object; actual APK manifest
grants/denials remain0/object and11/no object independently.

Package **122115058982** passes all23 actual installer gates. Both e2e
**122116078991 / 122116079057** pass all ten apps, four transports,
association/shortcut and **0 unexpected failures**; complete logs were read.
All10 format/38 media3/18 AOT3 and existing env/time/fresh-exit gates pass.
Original AAudio44100 frames/750+1250Hz/lost0/xruns0 remains strict. Controlled
small rings lose6311/6333 frames with xruns1, versus configured9839/9861
retained and lost0/xruns0 on2022/2025. The old1153.3Hz cause is not inferred.
The strict full-log verifier passes. No local native/JVM/C# compilation or
fixture execution ran.

The verified factor directly links pure audio/player-core/NDK OBJECT files
into both hosts with actual window/graphics dependencies. It shares the
exact old EnterLoadedCode/SetupThreadTls guard in the shim; JNI delegates.
The native recording provider defaults to deny; ART installs the unchanged
MayRecordAudio decision. No stub backend or permission bypass is added.
Native-child recording identity propagation and full configuration,
loss-of-control/output-mix/effects/device contracts remain open. Existing
34 IID verification objects stay pinned to their independently verified
d10 run; no historical field or143 format proof changes. Original accepted
integration db84749b is contained, and later commits change docs only.
Separate test78 AAudio MMAP source94e4a0e7/noticecf9240cc remains unverified
in wip/media4-aaudio (next test79); it is not in this frozen native source.

Corrected full ci **40476766790** is complete GREEN at
**d10bea3a98bb8dfe76972209b6aff5df4fed58ec**, attempt 1 without retries,
with accepted integration db84749b/freshExit30deb08c contained. All 28 job
conclusions were read: **22 success, six skipped**. Main122096814825 passes
in 2m47s (actual opensles.cc compile, both host links, 203 assembly
boundaries/zero problems); framework122096859804 passes6m13s, and
media122099370327 passes1m10s with both Werror NDK ABI libraries and all four
allowed/denied APK builds. Actual completed logs were read. Windows jobs
**122099872570** (2022,29m53s) and **122099872556** (2025,30m36s) each finish
**0 test(s) failed**. Each runner has14 test76 PASS lines: two ABIs through
core JNI and single/device allowed/denied APKs, plus four device shutdowns.
All50 actual semantic rows and40 set/get/value rows per runner were read:
53 canonical UUID objects/indirection/pinned fields; typed global/engine
queries, bounds/null/output rules and supported subsets; every ID's
required/optional and copied-ID outcomes; real PCM queue/volume/exposure;
actual recording permissions. Core denies with result11/objectfalse; real
manifest-allowed APKs return0/objecttrue and denied APKs11/objectfalse.
Each ABI reports libOpenSLES56 checked/0 missing. Only34 exact IID inventory
verification objects are added; the old baseline and all previous fields
stay intact. No effects, 3D, full configuration or standalone backend claim
follows from these constants and explicit unsupported outcomes.

Package **122099872538** passes all23 actual install/runtime/window/input/
uninstall checks. E2e **122100699777 / 122100699799** each passes all ten
required apps (Just Player0.217/Puzzles/TIC-80 included), four transport
fixtures, association/shortcut and **0 unexpected failures**. The complete
logs were read. Both suites preserve10 format,38 media3 (old37 plus the
controlled-ring check),18 AOT3, env67/time68 and actual exit-freshness70
checks; no full-suite failure is hidden. AAudio's original750/1250Hz and
44100-frame blocking gate passes single/device with capacity44100/lost0/
xruns0. The controlled small/configured rings show lost5613/5537 versus0,
xruns1 versus0 and configured retained9141/9065 on2022/2025. The earlier
1153.3Hz observation still lacks loss counters; this controlled result does
not retrospectively establish its cause.

The accepted source is frozen; later commits are docs only. Native peer's
bounded source review found no blocker, and all compilation/execution was
CI-only. The separate pure-backend continuation e5693412 (6be2eaec+d10)
is frozen under GREEN full40477536464 in C:\work\gesher-media4-native;
its independent full/runtime acceptance is recorded above. It moves the exact guest-thread TLS
entry guard into the shim, keeps the existing manifest provider/deny rule,
and adds genuine NDK ELF queue/TLS callback checks. It is not part of the
verified d10 source. Keep native-child recording identity, configuration,
loss-of-control/output-mix/effects/device contracts open. The installer also
needs the separate prepared OpenSL notice proposal applied and package-CI
verified: C:\work\gesher-ci-review\media4-opensl-notice-proposal.patch
(SHA256 f9c4197cf2200bc012dd2ce7e73b5a7dc70a01c59c3733f1982953dff03429d1).
This proposal is not applied to either frozen candidate.

Full ci **40470959941** is complete RED at
**2b343aa7cc25d441d36bcbc1b88ab2e7e827fa88**, after merging accepted
integration db84749b (docs above fresh-exit30deb08c). All28 conclusions were
read:20 success/six skipped/two failed. Main122077706841 passes3m28s,
framework122077748379 passes6m11s, and media122079941894 passes1m31s with
both actual NDK ABI builds and all four allowed/denied APKs. Package
122080555528 passes23 actual installer gates; e2e122081340170/136 each pass
all ten apps, four transport controls, association/shortcut and0 unexpected
failures. Actual completed logs were read. Windows122080555604/602 each
finish exactly6 test(s) failed, all in test76: both core-JNI permission
expectations and all five ARM volume round trips. Data/query/negotiation
parts pass in every context; actual APK allowed/denied permissions pass on
both ABIs, but no OpenSL inventory/roadmap acceptance follows from a red run.

Corrected source keeps the real core denial: jni_util's media VM cache is
unset without framework native registration, so AttachedEnv returns null
and MayRecordAudio denies. The previous fixture incorrectly expected a
grant. It now requires the actual0x0B/no-object denial independently of
manifest-allowed and denied APKs, without changing production permission.
ARM's signed16 volume boundary now receives a32-bit register value and
explicitly normalizes its low16 bits before comparison. AAPCS64 does not
promise the sign extension the host SysV short parameter could assume.
The old log lacks raw register/value diagnostics. The corrected GREEN run
now directly shows ARM -600 arriving as raw32=64936 and normalizing to
signed16=-600, with successful actual set/get/value for -600/-32768/-1/0 in
every context. This establishes the boundary assumption on that current
path without inventing the older register contents. No unchanged retry was
used. Native continuation e5693412 imports the same correction separately.

The AAudio78ee5e12 correction passes its actual lossless cases in this red
full run: both modes/runners read44100 original750/1250Hz frames with
capacity44100/lost0/xruns0. Controlled small rings lose5639/5572 frames with
one xrun on2022/2025 while configured rings retain9167/9100 frames with
zero loss/xruns. The old1153.3Hz observation remains without loss counters;
these new controlled results do not retrospectively prove its cause.

Frozen **wip/media4-format-verified** still points at docs-only
**ba3c1c2dc94e2e6e69c8e7c050df9dc3145c0317** above the accepted format
source below. The separate78ee5e12 lossless-capacity/diagnostic/probe slice
is now verified with OpenSL in full40476766790 above. Production buffering,
resampling and the separate three-cycle cumulative overrun check stay
unchanged. Test76 belongs to OpenSL; Exec4-spawn owns77; AAudio owns78; next free79.

Corrected full ci **40459815375** is complete GREEN at
**c5962291855b13ebd0d21fcf72981db973ad9e72**, after merging current
integration 53898b51's docs. Source 73c20d70 adds the explicit cstdint
prerequisite and the early exact-object CI compile. Source is frozen;
later branch commits change docs only. All 28 conclusions were read:
**22 success, six skipped**, attempt 1 without retries. Main build
122041490613 passes in 20m37s:
the actual log shows the early bridge object compiled, both gesher-exec and
dalvikvm linked, and 203 assembly boundaries/zero problems. Media build
122043516317 passes in 1m3s with both NDK ABI/ELF and APK builds; framework
122041532403 passes in 6m11s. Actual logs were read. Package 122048294220
passes all 23 installed-runtime/window/input/uninstall checks; boot-image
122048132669 passes in 28s. Windows jobs **122048294283** (2022, 28m24s) /
**122048294275** (2025, 26m22s) each finish **0 test(s) failed**. All ten
test71 PASS lines per runner were read: two ABIs, core ART JNI, standalone
ELF, single-process APK and fresh-device APK, plus both device shutdowns.
Every context prints all eight actual semantic parts, ABI and caller markers:
21 real functions, 152 real data objects/values, typed/FPU/LP64 calls,
ownership, errors, clear, debug-string lifetime and 128 allocation cycles.
The native2 availability check passes; each ABI reports libmediandk
276 checked / 133 missing. ADR 0064 is accepted for this format slice only.

Both complete suites also pass all 37 media3, 12 part-1 device, 18 AOT3,
11 Exec3, live-environ67 and monotonic/deadline-clock68 checks. Original
Camera2 throughput/picture/terminal/permission/race gates remain unchanged:
61/61 single-process frames and 58/59 device frames on 2022/2025, with zero
queue drops, rejects or encoder failures. Original startup gates pass:
interpreter 472/504 ms and JIT 347/386 on 2022; interpreter 387/421 and
JIT 302/339 on 2025. Actual deterministic HTTP/SNI certificate/hostname
controls and preserved Google Internet checks pass on both runners.
E2e **122048949119 / 122048949121** each passes all ten required apps
(including Just Player 0.217, Puzzles and TIC-80), four transport fixtures,
association/shortcut launches and **0 unexpected failures**. Actual package
and both e2e logs were read, not inferred from their conclusions.

The accepted source predates the coordinator's separately verified Java
exit-status freshness prerequisite (integration 30deb08c). The coordinator
will verify the coherent combined tree before integration acceptance;
this branch stays c596 plus documentation only until that handoff.

The first source candidate implements only the real NDK AMediaFormat
subset described in ADR 0064: 21 System V function entries and 152 actual
AOSP pointer data objects, in a small OBJECT target shared by dalvikvm and
gesher-exec. No dummy codec or format storage implementation is added.
The platform-library probe and native2 availability expectation now use
AMediaFormat_new, while test71 requires the unimplemented decoder to stay
absent. These two shared bionic/native2 edits are approved by the coordinator;
Net4's owner is coordinating its separate native2 changes and test72.

New mediaformatapp single-ABI APKs, core dex and independent ELF executables
share the same typed/data/ownership/error/lifecycle source for x86_64 and
arm64-v8a. Their build/publication is additive in media.yml; no new workflow
job or artifact download is needed. tests/windows/tracks/71-media4-format.ps1
owns the new checks, with bounded processes, fresh device roots and native
ABI/caller markers. All compilation and fixture execution are CI-only.

Full ci **40456299720** is complete RED at
**92e914238cc8655aef472887ad9f494eec5c8a97** (source 9f4dee5 after merging
integration 808ea746's docs): 15 successful, ten skipped and one failed job.
Main build 122030308024 fails on the new bridge at object 3093/3187:
NdkMediaFormat.h has 12 `unknown type name 'int32_t'/'int64_t'` errors with
mingw because it relies on bionic's transitive fixed-width definitions.
The correction includes cstdint before that header and adds an early CI
compile of the exact bridge object before the full AOSP graph. This is a
new-source correction, not an unchanged rerun; no native build runs locally.
PowerShell AST, XML/YAML, embedded-Python and Bash syntax/source checks pass;
the 21 registrations and 152 names/values match the pinned source exactly.
No compiler/native/JVM/C# fixture was run locally. Media job 122033620469
passes in 1m32s: its actual log shows Werror NDK builds of both ABI libraries
and independent ELF executables, and both single-ABI APKs/dex published.
Framework 122030408664 passes in 10m39s; actual NetApp/services dex logs were
read. Root independently reviewed the source/ABI/data/wiring and found no
actionable issue within that bounded review; actual CI exposed the header
dependency above. No Windows matrix, package or e2e job ran at that source;
the corrected candidate is accepted above. The native API baseline retains
its original fields and adds exact c596/40459815375 verification to only the
143 format entries. A scoped roadmap box records the real format slice;
the broad media box remains open. All remaining contracts listed above stay
open. No later Media4 implementation is included in this frozen source.
