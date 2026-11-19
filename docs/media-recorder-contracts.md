# MediaRecorder lifecycle and completed-file limits

Gesher adapts recording to Windows capture and encoder backends while preserving
Android's public operation order, codec selection, errors and notifications.
Normal builds use the real capture endpoint. Unavailable capture, codecs or
legacy Camera recording fail; they do not become successful silence, a different
codec or another kind of input. Isolated capture-test binaries remain separate
from ordinary applications and release packages.

The behavioral reference for this repair is **AOSP android-16.0.0_r4**:

- [MediaRecorder native state wrapper](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libmedia/mediarecorder.cpp)
- [MediaRecorder JNI exception mapping](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/media/jni/android_media_MediaRecorder.cpp)
- [Stagefright configuration/defaults/parameters](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libmediaplayerservice/StagefrightRecorder.cpp)
- [Java contract and asynchronous-limit documentation](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/media/java/android/media/MediaRecorder.java)
- [MPEG4Writer admission and approaching-limit callbacks](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libstagefright/MPEG4Writer.cpp)
- [WebmWriter](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libstagefright/webm/WebmWriter.cpp),
  [AACWriter](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libstagefright/AACWriter.cpp),
  [OggWriter](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libstagefright/OggWriter.cpp)

## State and errors

The JNI receiver lease retains and serializes public calls. Worker termination
and resource cleanup do not independently reset the public API state. In
particular, a limit notification leaves the native wrapper in RECORDING until
an application stop/reset; accepted pause/resume calls do not reopen the
completed session. A synchronous backend operation failure enters ERROR, and
reset or release is required before a new session.

| Operation | Behavior |
| --- | --- |
| Sources | Each source is set once, before output format. DEFAULT audio resolves to MIC. DEFAULT video resolves to CAMERA. |
| Output format | Requires a source; then enters the configured phase. Audio-only formats cannot have a video source. |
| Encoders | Require the corresponding source and output format, and can be set once. Unset and explicit DEFAULT are distinct. |
| Prepare order/presence failure | IllegalStateException without poisoning an otherwise configurable session. |
| Backend prepare failure | ERROR and IOException, except backend INVALID_OPERATION maps to IllegalStateException. An unset output descriptor is the latter. |
| Start/pause/resume/stop in the wrong public state | IllegalStateException. Repeated pause/resume within a recording session is accepted. |
| Backend start/stop/resume failure | ERROR and RuntimeException for the implemented non-INVALID_OPERATION failures. Failed capture restart terminates and joins workers; later stop cannot report success. |
| Stop with no accepted media | RuntimeException and ERROR. |
| Successful stop | Clears configuration and returns to IDLE. |
| Maximum duration | Positive values through 100 ms fail with RuntimeException; nonpositive disables. |
| Maximum file size | Positive values through 1024 bytes fail with RuntimeException; nonpositive disables. |
| Parameter failure | Nonfatal; a corrected parameter/setup is accepted. Semicolon-separated entries apply in order, as in Stagefright. |
| Output descriptor | Must be writable. Read-only argument rejection is nonfatal. A successful assignment truncates stale contents before the new recording. |

Telephony uplink/downlink/call, remote submix and FM-tuner inputs do not have
backends here and fail instead of recording the microphone. The manifest
RECORD_AUDIO gate is preserved. `setCamera(null)` throws NullPointerException;
legacy `setCamera` otherwise fails with the corresponding state/backend error.
The ordinary Surface path remains available for applications that actually use
it, including Camera2 sessions. Route queries return the default input only
while its capture stream is active, and do not invent an input for an idle,
paused, completed or video-only recorder.

## Requested codecs and capability bounds

The recorder supports explicit AAC-LC and Opus audio and the existing H.264
Surface-video path. AOSP DEFAULT audio selects AMR-NB; since Gesher has no
AMR-NB encoder, that request fails. AMR-WB, HE-AAC, AAC-ELD and Vorbis also fail.
Invalid enum values are rejected at JNI before they reach the backend.

AAC-LC is reported as the AAC encoder's implemented profile; HE/HEv2 decoder
profiles do not become encoder claims. Explicit unsupported AAC profiles,
including invalid nonpositive profiles, reach the factory and fail rather than
being replaced with LC. The MF backend also checks this at initialization.

Stagefright legitimately clips audio parameters against codec capabilities.
Gesher clips to the implemented bounds: AAC rates 44100–48000 Hz and bitrates
96000–192000; Opus rates 8000–48000 Hz and bitrates 6000–510000. Only the
implemented discrete rates are accepted inside those ranges: 44100/48000 for
AAC and 8000/12000/16000/24000/48000 for Opus. An interior unsupported rate fails;
values above the maximum clip to the maximum. The MF AAC encoder selects among
its supported bitrate settings. This does not authorize a different codec or
profile. These changes do not claim that every MediaCodec capability or recorder
extension is implemented.

## Accounting for the completed container

A maximum file size includes the final metadata and all retained media. Before
admitting another sample the writer evaluates the size of a completed file
containing that sample. A declined sample sets a distinct limit condition; it
is not counted as accepted media. The recorder stops capture/Surface input and
finalizes the accepted packets. A real write/finalization failure produces an
error, not a successful limit result. A mixed recording in which one configured
track has no accepted media also fails instead of reporting a complete recording
from the other track alone.

- **MP4/3GPP:** the estimate includes the file header, mdat bytes, each sample's
  stsz/co64 entries, stts duration runs, video sync samples, codec sample entries,
  delayed-track edit lists and optional location metadata. Each track retains a
  duration-run count and cached fixed sample-entry size. Admission visits the
  fixed set of tracks; it never serializes or traverses all earlier samples.
  The final tables are serialized once at stop. The controlled tests compare
  the exact budget against finalized one/two/three/seventeen-sample files as
  well as longer low-entropy and mixed-track files.
- **Ogg Opus:** the budget includes both headers, page/lacing overhead, the
  pending real packet that will receive EOS and the proposed packet. Header
  configuration is checked before it can enlarge the final output.
- **ADTS:** every access unit includes its seven-byte header in admission.
- **WebM:** the supported recorder route is a single Opus track. It uses actual
  file high-water bytes, a bounded initial header reserve, the next block and
  possible cluster, and the complete deferred cue budget. Multi-track and
  Vorbis WebM are not admitted as recorder size-limited paths merely because
  the general MediaMuxer writer can accept those track types.

For MP4, crossing the 90% estimate produces at most one
MAX_FILESIZE_APPROACHING (802), followed by one terminal MAX_FILESIZE_REACHED
(801) when the next sample cannot be admitted. The approaching callback is
posted outside recorder/muxer locks, while its worker still owns completion,
so the terminal callback cannot overtake it. A packet that jumps directly over
the limit need not have an earlier approaching callback. Other containers do
not borrow an MP4-only 802 policy. Maximum duration uses event 800.

### WebM bound and implementation boundary

Gesher uses libwebm's `mkvmuxer::Segment`, not AOSP's separate `WebmWriter`.
The pinned AOSP WebM source uses the requested maximum when reserving cue space;
it does not provide an incremental complete-size admission helper to reuse.
The implementation here follows the documented maximum-file contract using
libwebm's actual serialization behavior:

- A single audio track has no video interleave queue. Default
  `accurate_cluster_duration` is false, so SimpleBlocks are written immediately.
- The first-write reserve is `1024 + Track::Size()`. Track::Size includes the
  actual CodecPrivate data. The remaining EBML header, Segment header,
  five-entry SeekHead reservation, SegmentInfo and Tracks wrapper fit within
  1024 bytes with this writer's fixed settings/app names. No chapters/tags or
  chunking are configured.
- A new cluster's header is at most 22 bytes; a single-track SimpleBlock adds
  at most 13 bytes beyond its packet. A 64-byte reserve covers both. At most
  one new cluster/cue is needed per admitted packet on this route, including
  a large timestamp gap.
- Each CuePoint is at most 44 bytes for its bounded set of 64-bit integer
  fields and enclosing elements. The writer reserves 64 bytes per existing
  cue plus one possible new cue and up to 12 bytes for the Cues wrapper.
  `cue_entries_size()` is constant-time; no accumulated cue traversal occurs
  during admission.
- Finalize rewrites reserved headers/lengths in place and appends Cues. There
  is no pending single-track media to escape this budget. The writer tracks
  the high-water extent rather than payload bytes or a temporary seek offset.

The conservatism can end WebM below the requested maximum, particularly with
many clusters. It cannot license exceeding that maximum. The tests exercise
ordinary 20-ms packets and 40-second timestamp gaps that force frequent
clusters, then independently parse and decode the output.

## Reproducible gates and their boundaries

```bash
python3 tools/test_media_recorder_lifecycle.py
python3 tools/test_media_recorder_state.py --java-home /path/to/jdk
python3 tools/test_media_receiver_lifetimes.py --java-home /path/to/jdk
python3 tools/test_media_encoder_selection.py
python3 tools/test_media_mux_limits.py --libwebm /path/to/aosp/external/libwebm
```

The lifecycle gate compiles actual Recorder and writer translation units with
explicit capture/encoder/Surface fixtures. It independently decodes generated
AAC/H.264 outputs, tests successful reuse and failed-session reset recovery,
and uses actual `/dev/full` and RLIMIT_FSIZE errors on the host. The state gate
uses the real JNI namespace, receiver lease and Recorder on a checked host JVM;
only platform/backend boundaries and the host FileDescriptor field name are
adapted. The receiver-lifetime observer remains an ownership test, not a state
oracle. The encoder-selection gate uses the actual factory and exact extracted
capability function with backend creation observers.

The writer gate compiles all actual writer translation units and libwebm, feeds
real FFmpeg-generated AAC/H.264/Opus packets, and independently demuxes/decodes
limited MP4, 3GPP, Ogg, ADTS and WebM outputs. It tests 2 KiB, 8 KiB and 128 KiB
budgets (4 KiB for the smallest mixed-track case), irregular duration runs,
delayed tracks, video keys and deferred cues. The recorder gate separately checks
that a 2 KiB mixed recording fails when the audio exhausts its budget before
the Surface supplies a video frame.
These host checks do not execute Windows Media Foundation, WASAPI, ART, a
physical microphone/camera or the full installer.

The existing capture-failure adapter gate keeps its real capture/Start/AudioLoop
sources and declares its recorder coordination observers explicitly. Full
worker cleanup and error callback behavior are checked by the lifecycle and
state gates above. CI runs the host state/selection checks with its other JNI
gates and the lifecycle/writer checks after fetching the pinned libwebm source.

The `recordapp` Android-facing limit test waits for the specific terminal code,
checks 802 separately, and allows a bounded asynchronous-finalization interval
before opening/decoding the output. Its 128 KiB size case verifies a positive
completed length within the maximum rather than requiring an overshoot. The
existing Windows track consumes the matching markers. Those Windows/Android
runs remain a separate execution gate; compiling the Java test is not evidence
that they ran.
