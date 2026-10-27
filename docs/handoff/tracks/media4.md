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
also assigns 313 unresolved media symbols to media3: 276 libmediandk.so,
34 libOpenSLES.so and 3 libaaudio.so (ci 40296960603; both runners/ABIs).
media4 inherits these open exports; coordinate central per-entry ownership
updates with the coordinator, retain the recorded baseline, and add exact
subsequent verification rather than overwriting missing with a broad claim.
Read-only planning identifies 21 AMediaFormat functions and 122 format data
symbols in that baseline; the pinned AOSP implementation contains 152 data
definitions. A future bridge must verify all pinned entries and preserve
the distinction between a constant's value and the media contract it names.
None of these entries is implemented by the Media3 Java/recording proof.

## NDK export contracts

- Supply the native libmediandk codec, format, extractor, muxer, image/reader
  and protected-media interfaces, coordinating image/reader ownership with
  the devices/surfaces tracks. Java API round trips do not implement these
  ELF exports. Reuse functional backends and test NDK lifecycle, data and
  error behavior on both ABIs/runners; export resolution alone is insufficient.
- Implement the 34 remaining OpenSL interface identifiers together with
  their actual interface contracts or explicit documented unsupported
  results. Do not claim effects or 3D audio by exporting constants alone.
- Implement AAudioStream_isMMapUsed, AAudio_getMMapPolicy and
  AAudio_setMMapPolicy with correct Windows capability/policy semantics;
  verify getter/setter/result behavior rather than adding generic zeros.
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

Corrected full ci **40459815375** is running at
**c5962291855b13ebd0d21fcf72981db973ad9e72**, after merging current
integration 53898b51's docs. Source 73c20d70 adds the explicit cstdint
prerequisite and the early exact-object CI compile. Source is frozen;
docs-only commits may follow. Main build 122041490613 passes in 20m37s:
the actual log shows the early bridge object compiled, both gesher-exec and
dalvikvm linked, and 203 assembly boundaries/zero problems. Media build
122043516317 passes in 1m3s with both NDK ABI/ELF and APK builds; framework
122041532403 passes in 6m11s. Actual logs were read. Package 122048294220
passes all 23 installed-runtime/window/input/uninstall checks; boot-image
122048132669 passes in 28s. Windows jobs 122048294283 (2022) /
122048294275 (2025) and e2e 122048949119 / 122048949121 are running.
No test71/runtime or full-green acceptance yet.

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
the corrected candidate is tracked above. Read every job, both
complete zero-failure Windows summaries, all test71 parts, package and
required e2e before acceptance. The native API baseline metadata and roadmap
are unchanged. All remaining contracts listed above stay open; later work
must build on a CI-accepted slice.
