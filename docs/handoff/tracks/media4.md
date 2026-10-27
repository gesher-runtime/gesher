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
entries remain including the 34 OpenSL and three AAudio entries. Ordinary
format-key storage does not implement the media feature named by that key.

## NDK export contracts

- Supply the native libmediandk codec, format, extractor, muxer, image/reader
  and protected-media interfaces, coordinating image/reader ownership with
  the devices/surfaces tracks. Java API round trips do not implement these
  ELF exports. Reuse functional backends and test NDK lifecycle, data and
  error behavior on both ABIs/runners; export resolution alone is insufficient.
- Implement the 34 remaining OpenSL interface identifiers together with
  their actual interface contracts or explicit documented unsupported
  results. Do not claim effects or 3D audio by exporting constants alone.
  Read-only comparison with pinned OpenSLES.h / OpenSLES_IID.cpp also finds
  synthetic UUID contents in the existing 19 identifiers, pointer-identity
  lookup, ignored required-interface arrays and an incorrectly two-argument
  QueryNumSupportedInterfaces table entry (the API takes self/objectID/out).
  The local permission-denied value is 0x0D (INTERNAL_ERROR), while the
  pinned permission result is 0x0B. Correct canonical UUID values, copied-IID
  matching, capability/required-interface negotiation, the typed query ABI
  and NDK record-denial results need implementation and independent CI.
  These source findings have no runtime acceptance yet.
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
- Verify RECORD_AUDIO denial for NDK AAudio/OpenSL paths independently;
  current denial test proves the Java AudioRecord/MediaRecorder boundary.
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

Native continuation full ci **40477536464** is running at
**e569341230b3970cadaeb48a235a12a4645da74a**, after importing the signed16/
core-denial correction and resolving fixture/status conflicts with both
intents retained. Accepted integration db84749b is already contained.
Main122099364029 and framework122099455423 are pending. Bounded peer review
of the exact original factor and correction found no concrete source blocker;
compile/link/callback/TLS and both-runner runtime proof remain required.
This source is frozen. Corrected JNI/APK full40476766790 separately verifies
d10bea3a; neither candidate is accepted early and the old full remains RED.

Separate source-only continuation **wip/media4-native** lives in
**C:\work\gesher-media4-native**, cut from docs-only4d5f3532 above the
still-frozen OpenSL CI source2b343aa7 below. It factors pure audio, file/video
player and NDK OBJECT targets into both dalvikvm and gesher-exec, linking
functional hostgraphics/window dependencies without the JNI player. Native
callbacks use the original once-per-thread SetupThreadTls body moved into
the bionic shim; JNI delegates to that same entry. A pure recording gate
defaults to deny without an identity provider; ART installs the unchanged
MayRecordAudio manifest/bare-core decision. No manifest gate is bypassed.
The same NDK fixture additionally builds a real ELF executable and requires
native PCM callbacks with independent initialized guest TLS on both ABIs,
alongside all original JNI/allowed-denied APK contracts. Native processes
without a recording identity must return 0x0B with no recorder object.
Only source/diff and script parsing checks have run for this continuation;
no new compiler, native fixture or acceptance exists yet. The completed red
40470959941 remains on its original source and is not affected by this branch.
Native-child identity propagation, full configuration/effects/volume policy
and the other remaining Media4 contracts remain open.

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
The old log lacks raw register/value diagnostics, so that mechanism is not
claimed as proven yet. The corrected fixture preserves -600 and adds valid
-32768/-1/0 round trips; raw32/signed16 and actual set/get/result/permission
diagnostics are published. No unchanged rerun is planned. Native continuation
6be2eaec remains separate and unverified; it must import this correction.

The AAudio78ee5e12 correction passes its actual lossless cases in this red
full run: both modes/runners read44100 original750/1250Hz frames with
capacity44100/lost0/xruns0. Controlled small rings lose5639/5572 frames with
one xrun on2022/2025 while configured rings retain9167/9100 frames with
zero loss/xruns. The old1153.3Hz observation remains without loss counters;
these new controlled results do not retrospectively prove its cause.

Frozen **wip/media4-format-verified** points at docs-only
**ba3c1c2dc94e2e6e69c8e7c050df9dc3145c0317** above the accepted format
source below. The active branch now prepares a separate AAudio fixture
correction after Net4 full40462148835/719ae logs a Windows-2022 blocking
tone result of 750/1153.3 Hz, rather than 750/1250, with 44100 frames/exit0.
That old log has no blocking loss counters; overrun is a hypothesis, not
an established cause. The lossless blocking fixture now requests a one-second
capacity like its callback counterpart, reports effective capacity and
frame/xrun counters, and requires the original 44100 frames and unchanged
750/1250 frequency tolerance plus zero loss/xruns. Diagnostic-only native
open/read/stop traces expose coherent CaptureStats, including lost frames.
A bounded CI capture probe leaves 80ms/one-second rings unread until at
least 200ms of actual capture, requires real small-ring loss and configured
zero loss, and verifies the stopped rings drain their retained frames.
Production buffering/resampling and the separate three-cycle overrun check
are unchanged. No new compile/runtime proof yet; all execution remains CI-only.
OpenSL canonical IDs/query/negotiation/permission work owns new test76;
Exec4-spawn owns test77; next free test is78. It does not modify the frozen
format source. Source implementation now supplies 53 canonical UUID pointer
objects (19 corrected plus 34 old missing exports), value-based copied-ID
matching, typed engine query signatures, supported-subset enumeration,
required/optional array validation and explicit/prerealize exposure. Native
record denial now returns the pinned 0x0B rather than INTERNAL_ERROR 0x0D.
The new OpenSL fixture builds real NDK libraries for both ABIs and actual
manifest-allowed/denied APKs. Test76 requires all data/query/negotiation/PCM
queue-volume/permission parts in core JNI and single/device APK contexts,
with ABI markers and fresh per-variant device roots. This is source only;
no new native coverage entry or roadmap box is accepted before full CI.

Standalone OpenSL in gesher-exec remains unimplemented: its current backend
lives with JNI player/video/window dependencies. No launcher stub is added.
Full native backend factoring remains open, as do AndroidConfiguration's
ignored/unchecked options, loss-of-control callbacks, output-mix interfaces
and the broader OpenSL effects/device contracts. The fixture requires
explicit unsupported results for those identifiers; it does not claim effects.

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
