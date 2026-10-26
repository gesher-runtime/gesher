# Media: sound and video (roadmap phase 8, media track)

Design: [0042](decisions/0042-media-audio-engine-and-codecs.md). Tests:
`tests/windows/tracks/38-media.ps1` (dot-sourced by `run-tests.ps1`), with
`media_probe.exe` and `tests/apps/mediaapp` (`mediaapp.apk`, built by
`.github/workflows/media.yml` against the run's framework classes).

## Pieces

| Piece | Where |
|---|---|
| The audio engine: tracks (PCM 8/16/24/32-bit and float, any rate, 1..24 channels), a windowed-sinc resampler, the mix per stream volume group, events (markers, periodic positions, end, underrun) | `compat/media/audio/engine.{h,cc}` |
| Outputs: WASAPI shared mode (one render stream and Windows audio session per group in use; the session volume is the Android stream volume, the volume mixer's changes come back), a WAV capture (`GESHER_AUDIO_CAPTURE`), nothing (`GESHER_AUDIO=null`, or no endpoint) | `compat/media/audio/sink_wasapi.cc`, `sink_file.cc` |
| The device's shared audio state: stream indexes and mutes, master volume and mute, active streams, audio session ids, in a named mapping per `ANDROID_DATA` | `compat/media/audio/shared_state.{h,cc}` |
| `android.media.AudioTrack`'s natives | `compat/media/jni/audio_track_jni.cc` |
| `AudioSystem`'s (the output's rate and frame count, stream volumes, devices and routing, session and player ids, isStreamActive), the audio policy's (`AudioProductStrategy`), `AudioRecord`'s (no capture yet: setup fails) | `compat/media/jni/audio_system_jni.cc` |
| Stand-ins for every other native of these classes (zero, false, null), found by reflection | `compat/media/jni/jni_util.cc` (`RegisterStandins`) |
| Registration: one row in libandroid_runtime's table (`gesher_media`, `android::register_gesher_media`) | `compat/framework/android_runtime.cc` |
| The system server's `audio` service: stream volumes (aliases as on a phone, set, adjust, mute), the ringer mode, the audio mode, audio focus across apps (a stack: losses and the gain back), players' ids, isMusicActive | `app/java/com/gesher/server/AudioService.java` (registered in `Services.java`) |
| AOSP's extractors (MP3, MPEG-4, Ogg, FLAC, WAV, ADTS) and their foundation (libstagefright_foundation without IMemory, id3, esds, metadatautils, NdkMediaFormat), the open decoders (mp3dec, Tremolo, libopus, libFLAC), built for Windows | `cmake/aosp/media_codecs.cmake`; `aosp.lock` (`frameworks/av-media`, `frameworks/native-media`, `system/media`, `external/tremolo`, `external/libopus`, `external/flac`, `external/libwebm`); what bionic or Linux gave them: `compat/media/aosp` |
| Data sources: file descriptors (assets, raw resources, content URIs), paths, http(s) over WinHTTP (range requests, the Windows proxy and certificate store), MediaDataSource | `compat/media/player/data_source.{h,cc}`, `http_source.cc`, `jni/media_player_jni.cc` (`JavaDataSource`) |
| The extractor wrapper (the plugins' sniffers, the most confident wins; track and file formats as AMediaFormat) | `compat/media/player/extractor.{h,cc}` |
| Decoders: MP3, Vorbis, Opus, PCM, G.711 in-tree; AAC through Media Foundation's MFT (loaded at run time) | `compat/media/player/decoder.{h,cc}`, `mf_decoder.cc`, `decode_file.cc` |
| The player: libmedia MediaPlayer's states and rules, a playback thread (extractor, decoder, an engine track), seek, pause, looping, completion | `compat/media/player/player.{h,cc}` |
| MediaPlayer's, MediaMetadataRetriever's and SoundPool's natives, in the builtin `libmedia_jni.so`, `libsoundpool.so` (and `libaudioeffect_jni.so`: stand-ins) | `compat/media/jni/media_player_jni.cc`, `media_jni_others.cc`, `soundpool_jni.cc`, `media_jni.cc` |
| AAudio and OpenSL ES for apps' ELF libraries (bionic shim symbols) | `compat/media/ndk/aaudio.cc`, `opensles.cc` |
| Default ringtone, notification and alarm sounds (synthesized), installed as `$ANDROID_ROOT/media/audio`, the settings pointing at them | `compat/media/sounds/gen-system-sounds.sh`, `AudioService.putDefaultSounds`, `tools/package/gesher.iss`, `gesher-app.ps1` |
| Build | `cmake/aosp/media.cmake` (OBJECT libraries linked into dalvikvm; `media_probe`) |

## How a sound plays

`new AudioTrack(...)` → `native_setup` creates an engine `Track` (format,
rate, channels, the stream type from the attributes, the buffer size); writes
copy into its ring (streaming) or its static buffer; the engine's thread,
every 10 ms, mixes the tracks of each stream group at the output's rate,
resampling and converting channels, applies the track's volume (ramped) and
the master volume, and gives the mix to the sink. WASAPI applies the stream
volume as the session's; the capture applies it in software. Positions count
the source frames the resampler has passed; `stop()` drains a streaming track,
then it reports stopped.

Stream volumes: the system server's AudioService calls
`AudioSystem.initStreamVolume` / `setStreamVolumeIndex` (as it calls
AudioFlinger on a phone); the natives write the shared state, which every
app process's engine reads (a generation counter, checked every period). The
Windows volume mixer writes back to it (`IAudioSessionEvents`), so
`getStreamVolume` shows what the user set there. Indexes map linearly to the
session volume (index / max), which is what the mixer's slider shows. Gesher
starts every stream at its maximum, as Windows starts an app's session.

## How a file plays

`MediaPlayer.setDataSource` makes a data source (a file descriptor is
duplicated, as an asset's or a raw resource's `AssetFileDescriptor` is closed
right after; an http(s) URL is fetched at prepare, in 256 KB range requests);
`prepare` (or `prepareAsync`) starts the playback thread: every extractor's
sniffer is asked, the most confident opens the data, the first audio track
with a decoder is chosen, and `MEDIA_PREPARED` goes back. `start` creates an
engine track at the decoder's rate, channels and PCM format (the stream type
from the attributes) and the thread decodes into it; `pause` pauses the track;
`seekTo` reads from the new time (the extractor's seek), flushes the decoder
and the track; at the end the track drains, then `MEDIA_PLAYBACK_COMPLETE`,
or, looping, the reader starts over without stopping the track. Positions
come from the track's played frames, mapped to media time per segment (a
seek or a loop starts one). The states and their rules are libmedia
MediaPlayer's: a call in the wrong state gives `MEDIA_ERROR (-38, 0)` (start,
pause, ...) or an exception (setDataSource, prepare), as on a device.

SoundPool decodes a sound whole on its loader thread (`SAMPLE_LOADED` when
done) and plays each stream as a static engine track; MediaMetadataRetriever
reads the container's and the tracks' formats (tags, MIME type, duration,
sample rate, album art). Ringtones are MediaPlayers of the default sound's
file (`Settings.System.RINGTONE` and the rest point at Gesher's sounds).
AAudio's and OpenSL ES' streams and players are engine tracks (a data
callback, or the buffer queue's callback, called from a thread set up for
the app's ELF code) or players.

## Running by hand

- `set GESHER_AUDIO_CAPTURE=C:\some\dir\` (or a `.wav` path): the process's
  output goes to `audio-<pid>.wav` there instead of the speakers;
  `media_probe analyze file.wav` lists its sounds.
- `set GESHER_AUDIO=null`: nothing is heard (paced by the clock).
- `media_probe wasapi` checks the default endpoint (silent unless
  `GESHER_PROBE_AUDIBLE` is set); `media_probe mf` lists Media Foundation's
  decoders.

## Video (stage 2, ADR 0052)

- **Decoders** (`compat/media/video`): `VideoDecoder` gives I420 frames in
  presentation order. `mf_video.cc`: Media Foundation's synchronous decoder
  MFTs (H.264, and H.265/VP8/VP9/AV1/MPEG-4/H.263 where the machine has them),
  NV12 output converted to I420; `soft_video*.cc`: AOSP's software decoders for
  the rest ("Software video decoders" below). `VideoDecoder::Available()` is
  what MediaCodecList lists (Media Foundation's first; `GESHER_VIDEO_DECODERS=soft`
  lists the software ones only). `render.cc`: a frame into any Surface's
  ANativeWindow (RGBA, BT.601 limited range).
- **MediaCodec / MediaCodecList** (`jni/media_codec_jni.cc`): 8 input and 8
  output slots per codec as direct ByteBuffers, a worker thread, synchronous
  dequeue and the callback model; video output as tight I420
  (`COLOR_FormatYUV420Planar`, `getOutputImage` as YUV_420_888), audio as
  16-bit PCM (float when asked); codec-config buffers become csd-N; with a
  Surface, `releaseOutputBuffer(i, true)` draws the frame. Codec names
  `c2.gesher.<codec>.decoder`. MediaExtractor's H.264/H.265 formats carry
  csd-0/csd-1 as Annex B parameter sets (as NuMediaExtractor's do).
- **Surfaces without a compositor** (`compat/media/surface`, built into hwui):
  a SurfaceTexture's and a SurfaceView's Surface is a `QueueSurface` (frames
  stay in the process). TextureView's layer is Gesher's (HardwareRenderer
  `nCreateTextureLayer`, TextureLayer's natives, RecordingCanvas
  `nDrawTextureLayer` records a drawable that draws the latest frame);
  SurfaceView draws its surface's latest frame into its hole (frameworks/base
  patch 0300, `SurfaceView.nativeGesherDrawSurface`). Each frame invalidates
  the view. The surfaces track (ADR 0056, wip/surfaces) owns child surfaces
  from now on: this path is self-contained (one registration call,
  `register_gesher_media_surfaces`; the patch; `target_sources(hwui ...)` in
  media.cmake) for it to replace; the decoders only need an ANativeWindow.
- **MediaPlayer with video** (`player/player.cc`): a video thread shows each
  frame at its time on the media clock (the audio track's position, the wall
  clock without audio), drops frames over 100 ms late, shows the first frame
  after a seek at once; MEDIA_SET_VIDEO_SIZE, `getVideoWidth/Height`,
  `setSurface`/`setDisplay`. **MediaMetadataRetriever.getFrameAtTime** and
  `getScaledFrameAtTime` decode the frame at (or the sync frame around) a time.

### Software video decoders and the Matroska/WebM extractor
Media Foundation on the CI runners decodes H.264 and MPEG-4 part 2 only; the
other codecs apps meet come from AOSP's software decoders, built for Windows
(`cmake/aosp/media_video_codecs.cmake`; source lists from their Android.bp
files, third-party warnings silenced):

| Codec | MIME | Decoder (built from) | Test clip |
|---|---|---|---|
| VP8 | `video/x-vnd.on2.vp8` | libvpx (`external/libvpx`, generic C config) | WebM (and VP8 + Vorbis) |
| VP9 | `video/x-vnd.on2.vp9` | libvpx | WebM (and VP9 + Opus) |
| AV1 | `video/av01` | libdav1d (`external/libdav1d`, C only, 8 and 16-bit templates) | MP4 |
| H.265 | `video/hevc` | libhevcdec (`external/libhevc`, SSSE3/SSE4.2 intrinsics) | MP4 |
| H.264 | `video/avc` | libavcdec (`external/libavc`; media3) | MP4 (software-only run) |
| MPEG-4 part 2 | `video/mp4v-es` | m4v_h263 (frameworks/av `media/module/codecs/m4v_h263/dec`; media3) | MP4 |
| H.263 | `video/3gpp` | m4v_h263 (media3) | 3GP, 352x288 |

- `cmake/aosp/media_codecs.cmake`: `gesher_media_extractor(mkv ...)`, AOSP's
  MatroskaExtractor with libwebm's mkvparser, libstagefright_flacdec
  (FLACDecoder.cpp, over libFLAC) and ColorUtils_ndk.cpp built in;
  `compat/media/player/extractor.cc` lists `GesherExtractorDef_mkv`. WebM's
  Vorbis and Opus tracks go to the existing audio decoders.
- `compat/media/video/soft_video.cc` (libvpx, dav1d, libhevc),
  `soft_video_avc.cc` (libavc: its `common/ivd.h` is libhevc's name too, so a
  file of its own; libavc's `ithread_*` are renamed at compile time, as
  libhevc defines the same functions), `soft_video_m4v.cc` (m4v_h263). Each
  decoder gives 8-bit I420 frames in presentation order (deeper samples
  shifted down, 4:2:2/4:4:4 chroma subsampled, 4:0:0 gray chroma):
  - libvpx: one `vpx_codec_decode` per sample, the frames it shows carry the
    sample's time; Drain/Flush decode a null buffer.
  - dav1d: `max_frame_delay` 1, the picture's `m.timestamp`; Flush is
    `dav1d_flush`.
  - libhevc and libavc: the parameter sets (`AnnexBParameterSets`) go in front
    of the first access unit (libavc: in a call of their own); header decode,
    then frame decode (as C2SoftHevcDec/C2SoftAvcDec); the decoder reorders for
    display and returns each picture's `u4_ts` marker, mapped back to its time.
    Drain/Flush: flush mode until no picture is left, then a reset (parameter
    sets sent again).
  - m4v_h263: `PVInitVideoDecoder` with the VOL header (csd-0; none for H.263),
    then `PVDecodeVopHeader`/`PVDecodeVopBody` per sample into two frame
    buffers that take turns as the reference (as C2SoftMpeg4Dec); no B-frames.
- MediaCodecList names them `c2.gesher.<vp8|vp9|av1|hevc|avc|mpeg4|h263>.decoder`.
  Their capabilities use 8x8 blocks for H.265/VP9/AV1 (MediaCodecInfo scales
  the block counts by the codec's block size against the level limits').
- Verified (VP8, VP9, AV1, H.265 and the WebM extractor): ci run 40258938663
  (commit b57bb74, every job green), both Windows runners:
  `PASS  media: MediaCodec in videoapp.apk: VP8 (libvpx) from a WebM clip (MatroskaExtractor) decoded to I420 ByteBuffers and an Image; every frame in order, each quarter's color`
  and the same for VP9 (libvpx), AV1 (libdav1d) and H.265 (libhevc, B-frames
  reordered). (An earlier write-up named ci 40244779791: that run failed, a
  media check among its failures on both runners, the SkiaGL TextureView read
  back white, so it verifies nothing.)

### Stage 2 verified: ci run 40258833980 (commit 6aff13d)
Both runners, 0 failed; e2e 0 unexpected failures; origin/phase4/integration
9e449b0 merged. `tests/apps/videoapp` (clips of 2 s at 320x240, red, lime,
blue, white for 0.5 s each, generated by `gen-media.sh`):
- `PASS  media: MediaExtractor in videoapp.apk: an MP4's H.264 and AAC tracks, csd-0/csd-1 as Annex B parameter sets, samples, sync samples and times`
- `PASS  media: MediaCodecList in videoapp.apk: the decoders this machine has (H.264 through Media Foundation, the audio decoders), their capabilities; no encoders`
- `PASS  media: MediaCodec in videoapp.apk: H.264 decoded to I420 ByteBuffers and an Image (synchronous), with a callback (asynchronous), a seek with flush; every frame in order, each quarter's color; the AAC track to PCM`
- `PASS  media: MediaCodec into a TextureView's SurfaceTexture and a SurfaceView's surface in videoapp.apk; TextureView.getBitmap`
- `PASS  media: the window shows the TextureView's and the SurfaceView's frames where they are (hwui's CPU pipeline), the TextureView red, the SurfaceView blue (read back: texture view #fe0000, surface view #0000ff, background #808080)`
- `PASS  media: MediaMetadataRetriever.getFrameAtTime in videoapp.apk: the closest frame, the previous sync frame, a scaled frame`
- `PASS  media: MediaPlayer with video in videoapp.apk: the clip's H.264 into a TextureView with its AAC audio, the video size, the frames shown, completion, A/V sync (the frame shown matches the audio position within 120 ms); VideoView plays to its end`
- `PASS  media: the same with hwui's SkiaGL pipeline (ANGLE; its frame read with glReadPixels): the TextureView red, the SurfaceView blue (read back: ... texture view #fe0000, surface view #0000ff, background #808080`
- Also every stage 1 line again, the windows-2022 sound check included (the
  gap fix: AAudio's callback thread and OpenSL ES's queue feeder are woken by
  the mixer, `Track::WaitForRoom`, instead of polling with sleeps that wake
  15.6 ms late under load and let 40 ms buffers run dry).

### Stage 2 gaps
- `INFO  media: SkiaGL window as Windows shows it (PrintWindow): #ffffff ...`:
  an Activity window on SkiaGL shows nothing to PrintWindow (the whole window
  reads white) although hwui's GL frame is right: the graphics track's path
  (docs/angle.md "An Activity's UI on GL").
- The CI images' Media Foundation has H.264 and MPEG-4 part 2 only; VP8, VP9,
  AV1 and H.265 need the software decoders and WebM the Matroska extractor
  (wip/media-soft). DXVA hardware decoding, MediaMuxer, encoders,
  MediaCrypto/DRM, MediaCodec output to an ImageReader's surface, GLSurfaceView
  and TextureView.lockCanvas are not done. No F-Droid video app in e2e yet.

## Recording (stage 3, media3, ADR 0059)

| Piece | Where |
|---|---|
| The capture engine: a source (GESHER_AUDIO_SOURCE's WAV, `loopback`, the default capture endpoint, silence), streams at any rate, channels and PCM format | `compat/media/audio/capture.{h,cc}`, `capture_wasapi.cc` |
| AudioRecord's natives (setup refused without RECORD_AUDIO; reads, markers, periods, timestamps, the routed input) | `compat/media/jni/audio_record_jni.cc`; the permission: `AudioService.recordAudioAllowed` |
| The input device in AudioManager.getDevices (AUDIO_DEVICE_IN_BUILTIN_MIC, id 3) when there is one | `audio_system_jni.cc` |
| AAudio input streams, OpenSL ES audio recorders | `compat/media/ndk/aaudio.cc`, `opensles.cc` |
| Encoders: H.264 and AAC through Media Foundation's encoder MFTs, Opus through libopus, libavc's H.264 encoder where Media Foundation has none (`GESHER_VIDEO_ENCODERS=soft`) | `compat/media/encode` |
| MediaCodec encoders (ByteBuffers, Images, createInputSurface), MediaCodecList's encoders; decoders into an ImageReader's Surface | `compat/media/jni/media_codec_jni.cc`; input surfaces: `compat/media/video/input_surface.cc` (built into hwui) |
| Container writers: MPEG-4/3GPP, WebM (libwebm's mkvmuxer), Ogg Opus, AAC ADTS; MediaMuxer | `compat/media/mux`, `jni/media_muxer_jni.cc` |
| MediaRecorder: audio, video from its Surface (Canvas, a Camera2 session) | `compat/media/record`, `jni/media_recorder_jni.cc` |

Running by hand: `set GESHER_AUDIO_SOURCE=C:\some\tone.wav` makes that file
the microphone (looped, from its start each time recording starts);
`media_probe capture out.wav` records 2 s through the engine.

## Status and next steps

### media3 (stage 3) — stopped by the coordinator, 2026-10-26
- Running: ci 40299212277 at a1a0f43, with integration a98dda9. Verify the
  new session round trip, NDK tones, all existing media checks and Just
  Player's e2e; prior runs are recorded below for diagnosis.
- That run's build/package pass, but e2e still has 1 unexpected failure:
  Just Player now reaches a preallocated NoClassDefFoundError with no
  descriptor/stack. Windows test jobs are still running. Added opt-in ART
  patch 0590 to log missing android/com.android boot descriptors, enabled
  only by GESHER_TRACE_MISSING_BOOT_CLASSES in the e2e job, and included
  those lines in Just Player diagnostics. `git apply --check` against the
  saved ART source passes; this diagnostic patch is not compiled/CI-verified.
  Read both current test logs before the next dispatch, then merge current
  integration (native2, 8b1b09d or later) and fix/diagnose the actual missing
  dependency. Do not count package success as app/session verification.
- Windows results are now read: 5 failures on 2022 (badssl timeouts plus
  the two session checks), 3 on 2025 (known audio timing plus the two
  session checks). NDK read/recorder tones and H.264 picture counts now
  pass in both modes/runners. Session creation is the common remaining
  NoClassDefFoundError, also seen by Just Player. The pinned constructor
  unconditionally casts a null media_communication service to the optional
  mainline MediaCommunicationManager, whose runtime class is not shipped.
  Framework patch 0591 guards that cast when the service is absent; the
  test logs whether that optional class is present and still requires the
  complete legacy-session round trip. This is pending CI, not proof that
  the missing Session2/media module is implemented. Added its source path
  to aosp.lock for patch application.
- Lead-engineer continuation: ci 40292991000 at fc99140 completed with
  4 failures on windows-2022 and 5 on windows-2025; package passed, e2e
  had 1 unexpected failure. The recording/mux/recorder/ImageReader paths
  reached their checks. Both modes/runners fail the NDK read/recorder tones
  and count 61 encoded H.264 buffers instead of 60 pictures (one is codec
  configuration; decoding correctly returns all 60 pictures). windows-2025
  also found no WebM output tones; investigate that separately.
- Pending verification: AAudio now drains each capture burst when a read
  exceeds ring capacity, instead of waiting for the whole request and
  overflowing. CaptureEngine remembers an idle stop/start transition even
  when the next stream starts before its thread wakes, and discards a stale
  pull at that transition. The H.264 test counts data buffers separately
  from configuration while retaining the exact decoded frame/color checks.
- Just Player's diagnostic log identifies the launch failure: the platform
  MediaServiceManager was never initialized. Bootstrap now installs it and
  Services provides a Binder media_session service, generated owner/controller
  stubs, metadata/state/queue/extras, controller transport/callbacks, active
  tokens and owner-death cleanup. recordapp checks this in both modes.
  Local Gesher C++ syntax checks pass in standalone logging mode, and the
  test app compiles with JDK 21 against the saved framework classes. That
  saved class set lacks AudioAttributes, so the service's full local compile
  is unavailable (JDK 17 also rejected its version-65 class files); CI must
  compile and execute it. Shared edits: app/build.sh, Main.bootstrap,
  Services.java and 50-services.ps1's expected service lists.
- Session work still open: Windows system media transport controls, media-key
  receiver fallback, Session2/media communication, listener permission/user
  filtering and external audio-volume change notifications. The new session
  service is not claimed complete or verified.
- Continuation: read ci 40287172595's windows-2022 job 121463451415 and
  e2e job 121464252568. The MPEG-4 part 2 decoder produces no frames (1x1
  output); prepend csd-0's VOL to its first sample as well as setting the
  sequence-header attribute, and resend configuration after flush. Unverified.
- The test job stops before any media3 checks with a null-valued-expression
  error: 59-media3's no-endpoint branch overwrites PowerShell's $Matches with
  a second regex, then dereferences the discarded capture. Preserve the
  capture by ordering that regex last. Added track/line/stack diagnostics to
  the test dispatcher so future script exceptions remain visible in job logs.
- Just Player fails to reach its first frame (launcher exit 1); e2e has
  `1 unexpected failures`. Pinned its SHA-256 from that log and enabled its
  diagnostic log output: the first exception shown is a missing positions
  file, which alone does not establish the launch failure's cause. Stage 3
  remains unverified and needs a new full run after integration is merged.
- Running: ci 40292991000 at fc99140, after integration 7d6a82e (windows3).
  Read 38-media and 59-media3 on both runners, confirm that the test scripts
  reach their zero-failure summaries, and read Just Player's diagnostic lines
  in e2e. Do not infer stage 3 coverage from a successful build alone.
- Branch wip/media-soft. Last fully green run: 40258938663 (b57bb74, stage 2 +
  software decoders). In progress when stopped: **ci 40287172595** (the first
  build of all the stage 3 code that links; read its test logs first).
- ci 40262576561 (part 1 leftovers, commit before 0ca7cc2): built and ran; on both
  runners PASS for the soft-only run (libavc H.264, m4v H.263), H.263, WebM
  VP9+Opus, MediaPlayer/MediaMetadataRetriever over the software decoders, WebM
  audio reaching the output. FAILed: MPEG-4 part 2 (csd-0 missing: ESDS not
  converted; fixed since by `AddCsdFromEsds`, unverified) and the VP8+Vorbis
  WebM seek line (1003 ms; the app now rounds to 10 ms, unverified).
  media_probe mf: both runners have Media Foundation's AAC and H.264 encoders,
  no Opus/VP8/VP9/HEVC encoders.
- ci 40285626240 (0ca7cc2): everything compiled; link failed on mkvmuxer's
  MkvWriter (fixed: mkvwriter.cc added; that fix is what 40287172595 builds).
- Done, not verified: the capture engine (capture.h), AudioRecord, AAudio input,
  OpenSL ES recorder, RECORD_AUDIO, MediaCodec encoders (MF H.264/AAC, libopus,
  libavc fallback), input surfaces, MediaMuxer (MP4, WebM), Ogg/ADTS writers,
  MediaRecorder (audio; Surface; Camera2 through its Surface), decoder output
  into an ImageReader, recordapp + 59-media3.ps1 (single process and Gesher
  device), Just Player in tests/e2e/apps.json (sha256 empty: pin it from the
  e2e log's "sha256 ... (not pinned yet)" line), ADR 0059, the Recording section.
- Not started: VIDEO_SOURCE_CAMERA (legacy Camera recording), persistent input
  surfaces, VP8/VP9 encoders, 10-bit output, roadmap boxes for stage 3 (tick
  only what CI verifies).
- Next: read 40287172595's 38-media/59-media3 lines on both runners, fix what
  fails (expected weak spots: Camera2 accepting the recorder's QueueSurface
  (RGBA) as an output; MF AAC/H.264 MFT setup order; mkvmuxer frame ordering;
  exact durations in the regexes of 59-media3.ps1), then pin Just Player's SHA,
  merge origin/phase4/integration, a final run, docs and roadmap.
- Pitfalls: the Bash tool eats backslashes (write Python edit scripts with the
  Write tool); NetFree blocks `gh run download` of artifacts (compile-check test
  apps with javac 17 against gfx-local's framework-classes after rewriting class
  versions to 61: scratch script downver.py idea).

### Done and verified
- **CI run 40129230078** (ci, windows-2022 and windows-2025, 0 failed each;
  package and e2e green; commit da91170): stage 1, part 1 (the engine,
  AudioTrack, AudioSystem, AudioManager and the system server's AudioService):
  - `PASS  media: the audio engine plays 8/16/24/32-bit and float PCM, mono and stereo, 8000..96000 Hz, streaming and static tracks, a loop, markers and periodic positions (media_probe tones)`
  - `PASS  media: the engine's capture has each tone at its frequency, duration and level (resampled to 48000 Hz; volume 0.5 at half the level)`
  - `PASS  media: WASAPI fails cleanly where there is no audio endpoint (WASAPI: no audio endpoint (0x80070490))`
    (locally, with an endpoint: `wasapi: WASAPI shared mode on Speakers (...), 48000 Hz, 2 channels, float, 40 ms buffered`)
  - `PASS  media: AudioTrack in mediaapp.apk: streaming (short, byte, direct ByteBuffer writes) and static tracks, 8-bit/16-bit/float, mono and stereo, a marker, periodic notifications, loop points, setPlaybackRate, getTimestamp`
  - `PASS  media: AudioManager in mediaapp.apk: stream volumes (aliases, set, adjust, mute), the ringer mode, getDevices, audio focus (a transient loss, the gain back, a loss), isMusicActive`
  - `PASS  media: mediaapp.apk's AudioTracks reach the output: each tone at its frequency, duration and level, at its stream's volume (none while the ringer is silent)`
  - `INFO  media: Media Foundation: ... AAC decoders: 1; MP3 decoders: 1; FLAC decoders: 1; Opus decoders: 1; H.264 decoders: 1; HEVC 0; VP9 0; AV1 0; MPEG-4 part 2 decoders: 1`
    (both runners: Media Foundation is there on the CI images).

### Stage 1 verified: ci run 40193603079 (commit 32cbb82)
Every job green, both test runners 0 failed, e2e 0 unexpected failures;
origin/phase4/integration 6111f0d merged. Both runners print:
- `PASS  media: AudioTrack in mediaapp.apk: streaming (short, byte, direct ByteBuffer writes) and static tracks, 8-bit/16-bit/float, mono and stereo, a marker, periodic notifications, loop points, setPlaybackRate, getTimestamp, AudioTrack.Builder's defaults`
- `PASS  media: AudioManager in mediaapp.apk: stream volumes (aliases, set, adjust, mute), the ringer mode, getDevices, audio focus (a transient loss, the gain back, a loss), isMusicActive`
- `PASS  media: MediaPlayer in mediaapp.apk: MP3, AAC (M4A and ADTS), Ogg Vorbis, Opus, FLAC, WAV (16 and 24-bit, mu-law) from raw resources, assets, a path and http; prepare and prepareAsync, completion, pause, seekTo, looping, errors`
- `PASS  media: MediaMetadataRetriever and SoundPool in mediaapp.apk: tags, MIME types and durations of an MP3, an M4A and a FLAC; two sounds loaded and played, one at twice its rate`
- `PASS  media: Ringtone and RingtoneManager in mediaapp.apk: the default ringtone, notification and alarm sounds are Gesher's; the default notification plays`
- `PASS  media: AAudio and OpenSL ES from an app's NDK library (mediaapp.apk's libmedia_ndk_test.so): a data callback stream, a blocking-write stream, a buffer queue player, a player of an asset by file descriptor; no input streams yet`
- `PASS  media: mediaapp.apk's sounds reach the output (AudioTrack, AudioManager volumes, MediaPlayer, SoundPool): each tone at its frequency, duration and level, at its stream's volume (none while the ringer is silent)`
- (and the engine lines of run 40129230078 again)
e2e: `PASS  e2e: org.billthefarmer.siggen 1.37 (Signal Generator) installs, draws its first frame (not blank), reacts to input, closes and its process ends`
(display 800x400; clicks on SQU and MUTE changed 1825 and 4458 pixels).

### How it got there (runs)
- 40138223997 (6a88e72): link error `fopen_utf8` (libFLAC's compat.h on
  _WIN32); generators not executable. Fixed: `compat/media/aosp/flac/
  fopen_utf8.c`, `git update-index --chmod=+x`.
- 40139309325: mediaapp stopped at its first MediaPlayer with the boot class
  loader's pre-allocated NoClassDefFoundError: `setDataSource(FileDescriptor,
  ...)` goes through `FileUtils.convertToModernFd` to MediaStore, the
  MediaProvider module's class. Fixed with the placeholder
  `app/java/android/provider/MediaStore.java`. Signal Generator: landscape-only
  views (0x0 on a portrait display) and a one-frame AudioTrack buffer
  (Builder without setBufferSizeInBytes); fixed with `"display": "800x400"`
  in apps.json and a minimum buffer for streaming tracks. media_probe analyze
  got hysteresis (codec noise at a tone's ends added zero crossings).
- 40143354361: mediaapp did not compile (a duplicate local). 40191461359:
  every AOSP fetch failed: aosp.lock had `system/media` twice after the
  devices track's merge (two fetches into one directory); now one line with
  both tracks' patterns.
- 40191708589: windows-2022 only: the OpenSL ES buffer-queue tone had a
  ~20 ms gap (1517.8 Hz measured for 1568: a normal-priority feeder under
  load); Signal Generator's first click missed (the installed layout is 52 px
  lower than a local run's). Fixed: the OpenSL feeder and the AAudio callback
  thread run at THREAD_PRIORITY_HIGHEST, the player's at ABOVE_NORMAL;
  clicks at 600,168 and 600,213.

### Remaining gaps (stage 1)
- No screen orientation: a landscape-only activity gets the device's display
  as it is (Signal Generator runs on an 800x400 display in e2e). Graphics/
  windows domain.
- MediaStore is a placeholder: no media provider (queries of its content
  URIs find no provider).
- Only PCM AudioTrack (no compressed/offload), no audio effects
  (`libaudioeffect_jni` stand-ins), no MediaProfiles (CamcorderProfile).
- Audio input is stage 3.

### Exactly what is next
- Stage 2 is verified at 6aff13d (ci 40258833980, "Video" above); next: merge
  wip/media-soft (software decoders, WebM) once its run is green, then stage 3
  (capture). The older notes below are stage 1's.
Stage 2 (video), when the coordinator hands it over: MediaCodec and
MediaCodecList (audio decoding first: ExoPlayer apps use MediaCodec +
AudioTrack), MediaExtractor (written: `media_extractor_jni.cc`), MediaMuxer,
video to a Surface (SurfaceView/TextureView need the graphics track's
compositor), H.264 through Media Foundation (present on the CI images).
Stage 3: capture (AudioRecord's setup fails cleanly now; AAudio input answers
UNAVAILABLE; OpenSL recorder FEATURE_UNSUPPORTED).

### Debugging with a run's artifacts (no local build)
`gh run download <run> -n windows-x64 -n framework -n graphics -n angle -n
boot-image -n media -n native-libs` into one dist/, set ANDROID_ROOT etc. as
run-tests.ps1 does (framework-res.apk, build.prop, boot image, fonts), then
`dalvikvm -Xbootclasspath:... -Ximage:... com.gesher.app.Main <apk>
[--hidden-window]`, with GESHER_AUDIO_CAPTURE set (else it plays on the
speakers). A changed class on the boot class path can be tried by appending a
d8-dexed jar to -Xbootclasspath (the image still loads); a test Activity can
be swapped into mediaapp.apk's classes.dex (javac against small stubs, d8).

### Known pitfalls
- The Bash tool eats backslashes in inline scripts (`\\a` became a BEL in
  gesher.iss once; `\\n` became a newline): edit with the Edit tool or Python
  scripts written to files; check for control characters.
- Python on Windows writes CRLF unless `newline='\n'`.
- `workflow_dispatch` only works for workflows on the default branch: run
  media.yml through ci.
- GitHub API rate limit is shared by every agent: poll with
  `--interval 120`, fetch logs once.
- Static initializers (builtin libraries, bionic shim symbols) need OBJECT
  libraries, or the linker drops them.
- Natives are System V (`JNICALL`); the NDK callbacks the shim calls are
  System V function pointers, on threads that call `EnterLoadedCode()` first.
- CI runners have no audio endpoint (`0x80070490`); MF is present.

### Edits outside the media domain (all additive)
- `CMakeLists.txt`: `include(cmake/aosp/media.cmake)`.
- `compat/framework/android_runtime.cc`: one declaration and one table row
  (`gesher_media`, `android::register_gesher_media`).
- `.github/workflows/ci.yml`: the `media` job (`uses: media.yml`, needs
  framework); `test` needs `media` and downloads its artifact; `package`
  needs `media` and downloads it (the default sounds).
- `app/java/com/gesher/server/Services.java`: the `audio` registration now
  uses `AudioService.java` (the old inner silent `AudioService` class removed).
- `app/java/com/gesher/server/SettingsProvider.java`: one line,
  `AudioService.putDefaultSounds(system)`.
- `tools/package/gesher.iss` (media\audio\*) and `tools/package/gesher-app.ps1`
  (copies them to `$ANDROID_ROOT/media/audio`): the installer edit.
- `aosp.lock`: the new projects above (separate checkouts), and the audio
  patterns added to the devices track's `system/media` line.
- `docs/roadmap.md`: the Phase 8 checklist.
- `app/java/android/provider/MediaStore.java` (new): the MediaProvider
  module's MediaStore as a placeholder on the boot class path (no
  transcoding, no scanning, the content URIs), as ADR 0034's module
  placeholders.
- `tests/e2e/run-e2e.ps1`: an app's optional `"display": "WxH"` runs it on a
  device of its own with that display (GESHER_DISPLAY); `tests/e2e/apps.json`:
  Signal Generator and the `display` field in the comment.
- Stage 2: `patches/frameworks/base/0300-media-surfaceview-frames-without-compositor.patch`
  (SurfaceView draws its surface's frames); `aosp.lock`: SurfaceView.java in
  frameworks/base's sparse set; `compat/media/surface` compiled into hwui
  (`target_sources(hwui ...)` in media.cmake) and registering natives of
  SurfaceTexture, TextureView, TextureLayer, HardwareRenderer's texture layer,
  RecordingCanvas.nDrawTextureLayer, BLASTBufferQueue (update/getSurface/destroy)
  and Surface (createFromSurfaceTexture, getFromBlastBufferQueue) over the
  graphics stand-ins.
