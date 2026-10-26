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
| `AudioSystem`'s (the output's rate and frame count, stream volumes, devices and routing, session and player ids, isStreamActive), the audio policy's (`AudioProductStrategy`); AudioRecord is described under Recording below | `compat/media/jni/audio_system_jni.cc` |
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
  AV1 and H.265 use the software decoders and WebM the Matroska extractor.
  Stage 3 below adds scoped MediaMuxer/encoder, ImageReader output and
  video-app e2e coverage. Hardware decoding and MediaCrypto/DRM remain open;
  shared surface contracts belong to the surfaces track. The full current
  boundary is recorded in [the API inventory](media-api-inventory.md).

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

### media3 (stage 3) — camera throughput gate remains open, 2026-10-26
- Prepared after source 6293a20 (not part of its running CI): request
  availability, selection and frame numbering now share one mLock section.
  Previously cancelRequest/flush could clear mRepeating between the two lock
  sections and cause a NullPointerException. A concurrent actual-class
  regression reproduces that exact failure against 6293a20; the repaired
  class passes 1,000 contended cancel/flush selections, ordered frame numbers,
  queued-capture priority and disconnected stop on the JDK. Its additive
  Windows check makes the next expected media3 count 36; full CI is pending.
- Latest full CI **40386129131** at **6293a20ee08173d17bf3c643457d14b68ad74f9e**:
  terminal callback fix 6722364 plus native phase diagnostics 7c5024a.
  Integration 634a273 is included. All 28 job results are read: 20 success,
  six skipped and two failed runtime jobs (2022 **121797816741**, 2025
  **121797816842**). Each runner has 34 media3 PASS lines and exactly
  `1 test(s) failed`, the original device camera recording gate: **21/29
  encoded and independently decoded frames**, versus 61/60 in single process.
  Every accepted frame is encoded/muxed with zero queue drops or encode loss.
- Both new `Camera2 close delivers onClosed exactly once before its callback
  thread quits` checks pass on each runner. The terminal five-second lock
  cycle is removed: capture loops now end near collection completion, and
  started/result callback phase maxima are below 272 us in both modes.
  This terminal repair does not close the active throughput failure.
- Native phase evidence now locates the device slowdown in JNI array access,
  rather than MF: 2022 SetByteArrayRegion totals/maxes **1,594,570/153,561 us**
  across 22 reads, while MF capture totals 375,362 us (ReadSample 19,055 us).
  2025 has **958,136/104,695 us** across 30 reads, while MF totals 411,861 us
  (ReadSample 29,525 us). Single-process copies total only 480/357 us.
  Delivery also spends 157,450/436,574 us in ReleaseByteArrayElements
  (JNI_ABORT). Pinned ART source uses ScopedObjectAccess, array validation
  and memcpy/free here; entry, body and exit waits are not yet separated.
  No GC, JIT, scheduler or native crash cause is claimed from these totals.
- Current package **121797816925** passes installed oatdump, app window/input
  and uninstall cleanup. E2e **121798915469** (2022) and **121798915169** (2025)
  each pass all four pinned transport fixtures, ten apps including Just Player,
  TIC-80 and Puzzles, association/shortcut launch and `0 unexpected failures`.
  Boot profile-vs-speed criteria pass (2022 JIT 421/436 ms, 2025 467/473 ms).
  This is a red full run, not a mergeable source or broader API claim.
- Previous full CI **40377453079**, attempt 1, exact source SHA
  **ad004619c279877dfbfa2927b0cc1114a9c8e7a0**, includes integration
  634a273 (README-only above verified oatdump integration f5ce6f64). All
  28 job conclusions and both runtime logs are read: 20 success, six skipped,
  two failed jobs. Windows-2022 **121769036527** and Windows-2025
  **121769038194** each end `1 test(s) failed`: only device camera throughput.
  Each passes **32 media3**, **12 part-1 device**, 32 surfaces, five native3,
  14 AOT2 and four AOT3 checks. There is no fully green claim.
- The real GESHER_CAMERA_TEST -> Camera2 -> recorder Surface -> H.264 MP4 ->
  independent decoded-picture pipeline records **61 frames in single process**
  on both runners, **20 device frames (2022)** and **27 (2025)**. Every accepted
  frame is encoded/muxed, with zero rejected frames, queue drops, encode
  failures or pending frames; drain succeeds. Picture/order remain correct.
  Original recording duration (2 seconds), 30+ frame requirement and all
  picture/tone/length thresholds remain unchanged.
- Opt-in phase evidence distinguishes active throughput from teardown. Device
  native-read phase totals/maxima are **1,720,977/182,060 us (2022)** and
  **1,329,584/131,342 us (2025)**. Delivery totals are 22,260/621,418 us;
  2022 also has one 158,542 us metadata operation. The 2022 result callback's
  5,039,719 us maximum is terminal work after collection, not five seconds of
  active production. Single-process traces also have terminal ~5 s started
  callbacks. Native/JNI sub-phase diagnostics are prepared to distinguish
  MF pacing/ReadSample/conversion/copy from JNI and delivery waits.
- A separate terminal lock cycle is established by the pinned framework's
  bytecode: CameraDeviceImpl.close holds mInterfaceLock across disconnect;
  its local started/result callbacks acquire that lock; CameraDeviceUser
  joins the capture thread for 5 s inside disconnect. Fix 6722364 queues
  callbacks serially, as the interface's FLAG_ONEWAY proxy does, retaining
  arguments/order and logging target errors. It drains that queue without
  joining the callback executor; the capture thread alone closes its native
  handle after its last read. The fixture now waits for exactly one onClosed
  before quitting/joining its callback HandlerThread, with a separate PASS
  assertion in each mode. Those terminal checks pass in 40386129131; the
  separate active frame-throughput gate remains open.
- Own six Java service sources and RecordActivity compile with release 11;
  a focused real-dispatcher lock/order/error/lifetime check passes on the JDK,
  59-media3 parses, and the portable native trace header passes C++17
  -Wall/-Wextra/-Werror syntax checking. Native AOSP-facing builds remain CI-only.
- All four AudioRecord mode/runner cases report zero lost frames/overruns.
  NDK callback/read and cumulative input-xrun, Binder session, codec/mux,
  audio/Surface recorder, permission-denial and ImageReader checks pass.
  The complete Process regression passes on both runners; both modes finish
  without uncaught app/daemon exceptions. The earlier uninstrumented audio
  frequency failure remains unexplained; no signal/buffer threshold changed.
- Boot profile-vs-speed startup passes this run: 2022 JIT **419/429 ms**,
  interpreter 555/582; 2025 JIT **463/491 ms**, interpreter 646/689. The original
  10%/30 ms criterion is unchanged. Previous 40368511782 failed 2022 JIT
  **559/476 ms**, and the APK-vs-app-image artifact fix is not a demonstrated
  cause of the changed boot timing. The coordinator owns repeat boot timing
  diagnosis; retain the previous failure rather than claiming a known flake.
- Main build **121764302707** compiles Process/native_fonts and links both
  oatdump/dalvikvm. Media **121768388471** builds all three APKs/NDK libraries.
  Actual package **121769036621** passes installed oatdump inspection,
  window/input and device/data/association/toast/Start-menu uninstall cleanup.
  E2e2025 **121770110767** / e2e2022 **121770111046** each pass four pinned
  transport fixtures, all ten apps (Just Player 0.217 and required TIC-80
  1.01.00 included), association/shortcut launch and `0 unexpected failures`.
- Previous 40368511782 at 43cfe4e had 2025 zero failures/33 media3 PASS, 2022
  two failures (device camera 16 frames and boot JIT timing)/32 media3 PASS.
  40359757434 at 20b1d38 exposed four missing Process stub declarations plus
  device camera (19/22 frames); root's complete 644e7f8 follow-up is now
  independently verified. 40349479693 had videoapp fixture uninstall failures
  (fixed by its own uninstall) plus camera; 40330465222 had an uninstrumented
  AudioRecord failure and camera on 2022. Output prefill/separators preserve
  tone thresholds and pass repeatedly. 40329138943/40343220358 were superseded;
  the latter's quiet apt setup is now bounded. No generator assets changed.
- Keep the [source API inventory](media-api-inventory.md), preserved
  [313 missing NDK exports](handoff/native-api-coverage.json) and
  [media4](handoff/tracks/media4.md) (ADR 0064 reserved). Ordinary H.264 actually
  uses libavc after MF I420 rejection; successful MF/forced-soft coverage,
  physical audio routes, legacy Camera, recorder lifecycle/limits, output
  cumulative-xrun cycles, WebM video/audio interleaving, Java DRM/crypto,
  session/Windows controls and permission/user filtering remain explicit
  follow-ups. services3 owns advanced Camera2 contracts. No broad roadmap
  completion box is ticked; the coordinator transfers the 313 entry owners.
- Next: separate the slow JNI calls' entry/body/exit with the coordinator,
  then include the atomic request-selection repair and current integration
  in full CI. Preserve collection duration and every acceptance threshold.
  No integration/main merge or release action here.

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
