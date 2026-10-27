# Track brief: media3

Media leftovers after stage 2 (ADR 0052): the software decoders left on
wip/media-soft by a helper agent that stopped, and stage 3. The original
brief given to the first agent of this track (2026-10-26), kept verbatim. A
successor reads it together with the "Status and next steps" section in
docs/media.md on its branch.

You are the "media3" track agent of the gesher project. Your worktree is
C:\work\gesher-media-soft on branch wip/media-soft (it holds a helper
agent's work, now yours). Work only there, with absolute paths under
C:\work\gesher-media-soft (check `pwd` before editing).

FIRST read docs/handoff/track-rules.md and follow it exactly: it is binding.
Then read docs/media.md (stage 1 and the "Video" section), ADR 0042 and ADR
0052, tests/windows/tracks/38-media.ps1, and `git log` of wip/media-soft
against origin/phase4/integration.

State you inherit: wip/media-soft at b57bb74 (ci 40258938663, all jobs
green) builds libvpx, libdav1d, libhevc and a Matroska/WebM extractor; the
VP8, VP9, AV1 and HEVC clips decode (60 frames, ordered, the right colors)
but 38-media.ps1 reports them only as INFO lines, not checks. Its
docs/media.md claims verification by ci 40244779791, which FAILED; its
docs/roadmap.md predates stage 2's merge.

Update (coordinator, same day, before this agent started): the helper did
report: 38-media.ps1 checks the four codecs (PASS lines on both runners in
ci 40258938663), and b57bb74 is merged into phase4/integration with the
roadmap box ticked. Part 1 is therefore reduced to its leftovers:
- Merge origin/phase4/integration into wip/media-soft first.
- WebM audio (VP9+Opus, VP8+Vorbis clips through MediaExtractor and
  MediaPlayer), MatroskaExtractor seeking, the software decoders through
  MediaPlayer and MediaMetadataRetriever as PASS lines; libavc / MPEG-4 /
  H.263 software fallbacks where the runners' Media Foundation lacks them;
  fix docs/media-soft-video.md's claim of verification by ci 40244779791
  (it failed overall) by folding it into docs/media.md.
GOAL, part 2 (stage 3; ADR docs/decisions/0059-media3-recording-and-encoders.md,
0059 is reserved for you):
- Audio input: AudioRecord (WASAPI capture; the runners have no
  microphone: use a loopback or a virtual capture source CI can feed, and
  record the extent), AAudio/OpenSL ES input, the RECORD_AUDIO permission.
- Encoders: MediaCodec encoders (AAC; H.264 through Media Foundation's
  encoder, software fallback if the runners lack it; Opus) and MediaMuxer
  (MP4, WebM); MediaRecorder for audio and for video from the camera
  (GESHER_CAMERA_TEST's camera on the runners) and from a Surface input.
- MediaCodec output into an ImageReader's surface, and an F-Droid video
  app (or a video-player app) in e2e, pinned by SHA.
Tests in 38-media.ps1 (or a new tests/windows/tracks/59-media3.ps1), both
runners, single process and on the Gesher device; e2e must stay green. Your
file domain: compat/media (except compat/media/surface, which the surfaces
track owns now), the media natives and Java services, media test apps
(framework.yml: one line per test app), 38-media.ps1 / 59-media3.ps1,
docs/media.md, ADR 0059, the media roadmap boxes.

Coordinator notes: CI first; NEVER build AOSP locally. Other tracks run in
parallel on the shared CI (windows3, aot2, exec2, surfaces, native2, web3):
trigger CI only when code is ready, cancel superseded runs. Watch with `gh
run watch <id> -R gesher-runtime/gesher --exit-status --interval 120`; on
HTTP 403 back off ~10 minutes. Keep a "Status and next steps" section
updated with every meaningful push. Before each final CI run, merge
origin/phase4/integration into your branch. Final message = the report
track-rules.md asks for (run ids, PASS lines, remaining gaps, edits outside
your domain, the verified SHA).

## Status and next steps

The current candidate is source-only d3dd4319, incorporating verified
integration 8922a3e and the immutable reviewed Exec4 environment prerequisite
76b8d5c. The HTTP/SNI implementation 4032be6a completes the deterministic
positive fixture and real missing/unknown-SNI controls; the exact NetApp
slice d50f6e7b is also present. Root review is pending before one new full
CI. No compiler/native/JVM/C# fixture runs locally. See docs/media.md and
C:\work\gesher-ci-review\media3-positive-tls-reviewed.md/.json for exact
source, patch hashes, preserved internet checks and pending verification.

Source 0148698 / full ci 40438975894 remains RED: Windows-2022 has zero
failures, Windows-2025 has exactly five external-network failures. Both
runners pass all 37 media3 and 12 part-1 device checks, unchanged camera/
tone/boot gates; package and both required ten-app e2e logs pass. This scoped
evidence does not permit merge until all full-CI conclusions are green or
intentionally skipped and both Windows summaries have zero failures.
