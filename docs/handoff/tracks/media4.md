# Track brief: media4 — remaining media contracts

Status: open follow-up to media3, 2026-10-26. ADR **0064** is reserved for
media4 by the coordinator; the implementing owner writes it when making
the next design decision. The coordinator assigns an
owner/worktree before implementation. Read docs/handoff/track-rules.md,
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

## Verified boundary

media3 implements file-fed audio input, AAC/H.264/Opus encoding, MP4/WebM
muxing, audio/Surface/Camera2 MediaRecorder and ImageReader decoder output.
The exact verified run and remaining red check are in docs/media.md. CI uses
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
- Exercise the software H.264 encoder with GESHER_VIDEO_ENCODERS=soft on
  both runners; current stage-3 encode checks select Media Foundation.

## Deliberate feature gaps requiring separate implementation

- Legacy MediaRecorder VIDEO_SOURCE_CAMERA/setCamera recording (Camera2
  into getSurface is the supported camera recording path).
- Persistent codec input surfaces and their detach/reuse lifecycle.
- Encoders beyond AAC, Opus and H.264: AMR, Vorbis, VP8/VP9/HEVC and any
  advertised format must be implemented before being listed as available.
- Hardware DXVA/D3D11 decode/encode, encoder B-frames and 10-bit output.
- Input audio effects (AEC/AGC/noise suppression), output effects/offload,
  MediaProfiles/CamcorderProfile and a real MediaStore provider.

## Completion evidence

Use own media tests with distinct PASS lines, both Windows runners and
single-process/device modes for runtime behavior. Keep e2e green and pin
new third-party apps. Merge current integration before final CI. Report the
exact SHA, job ids, PASS lines and unsupported contracts; do not tick broad
roadmap boxes based on one app launch or successful packaging. Shared files
must have minimal additive edits and be listed in the final report.
