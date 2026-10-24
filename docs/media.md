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
| Build | `cmake/aosp/media.cmake` (an OBJECT library linked into dalvikvm; `media_probe`) |

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

## Running by hand

- `set GESHER_AUDIO_CAPTURE=C:\some\dir\` (or a `.wav` path): the process's
  output goes to `audio-<pid>.wav` there instead of the speakers;
  `media_probe analyze file.wav` lists its sounds.
- `set GESHER_AUDIO=null`: nothing is heard (paced by the clock).
- `media_probe wasapi` checks the default endpoint (silent unless
  `GESHER_PROBE_AUDIBLE` is set); `media_probe mf` lists Media Foundation's
  decoders.

## Status and next steps

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

### Next (stage 1, part 2)
- AOSP's extractors (MP3, MPEG-4, Ogg, FLAC, WAV, ADTS) and the open codecs
  (mp3dec, Tremolo, libopus, libFLAC) built for Windows (`aosp.lock`:
  `frameworks/av-media`, `system/media`, `external/tremolo`, `external/libopus`,
  `external/flac`; `cmake/aosp/media_codecs.cmake`); AAC through Media
  Foundation's decoder.
- A player over them (`compat/media/player`: data sources for file
  descriptors, paths and http(s), the extractor wrapper, the decoders, the
  playback thread) and MediaPlayer's, SoundPool's and
  MediaMetadataRetriever's natives (libmedia_jni and libsoundpool as builtin
  libraries); Ringtone/RingtoneManager defaults; test media generated in
  media.yml with ffmpeg; an http check with a local server.
- Then AAudio and OpenSL ES for NDK code; stage 2 (MediaCodec,
  MediaExtractor, MediaMuxer, video into SurfaceView/TextureView through
  Media Foundation's H.264 decoder); stage 3 (capture).
