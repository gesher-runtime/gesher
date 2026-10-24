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

### Runs in flight
- None. Last run: **40135129611** (commit 882a1af, "the player ..."): build,
  framework, media (apk), package green; its `media / codecs` job (the AOSP
  extractor/codec port, `ninja -k 0 media_codecs_all`) **failed**, so the test
  jobs were skipped (fixed since: the codecs job is `continue-on-error`). Its
  log could not be read (GitHub API rate limit); the errors seen in the run
  before it (40134179211) were all addressed in commit 14bd96f and before:
  Tremolo's `_G_config.h`, FLAC's dllimport (`FLAC__NO_DLL`) and Linux
  `config.h` (Gesher's own in `compat/media/aosp/flac`), `vasprintf`/
  `strcasestr` and missing `<new>`/`<vector>` (force-included
  `compat/media/aosp/include/gesher_media_aosp.h`), `OSCL_UNUSED_ARG`.
- Last CI-verified commit: **da91170**, ci run **40129230078** (stage 1, part
  1, above; both runners green). Commits since (7b87bf0 docs, then the codec
  port, the player and its tests) are not verified.

### In progress (committed, not yet CI-verified): stage 1, part 2
Everything below is pushed (tip of `wip/media`), built only by media.yml's
`codecs` job until it compiles:
- `cmake/aosp/media_codecs.cmake` + `aosp.lock` (new entries
  `frameworks/av-media`, `frameworks/native-media`, `system/media`,
  `external/tremolo`, `external/libopus`, `external/flac`, `external/libwebm`):
  the extractors and decoders, `EXCLUDE_FROM_ALL`, target `media_codecs_all`.
- `cmake/aosp/media.cmake`: `gesher_media_player` (OBJECT, `EXCLUDE_FROM_ALL`):
  `compat/media/player/*`, `jni/media_jni.cc` (builtin `libmedia_jni.so`,
  `libsoundpool.so`, `libaudioeffect_jni.so`), `media_player_jni.cc`,
  `media_jni_others.cc` (MediaMetadataRetriever + stand-ins),
  `media_extractor_jni.cc`, `soundpool_jni.cc`, `ndk/aaudio.cc`,
  `ndk/opensles.cc`. Locally checked with clang `-fsyntax-only` (the parts
  that need no AOSP headers).
- Test side, ready: `tests/apps/mediaapp` (MediaPlayer, retriever, SoundPool,
  Ringtone, NDK AAudio/OpenSL ES via `jni/media_ndk_test.c`),
  `gen-media.sh` (ffmpeg; checked locally), `compat/media/sounds/
  gen-system-sounds.sh` (checked locally), media.yml steps (ffmpeg, NDK
  cache `ndk-27.2.12479018`, `.so` build), `38-media.ps1` expectations
  (MediaPlayer, retriever, SoundPool, ringtone, NDK lines and 31 sounds; an
  http server on 127.0.0.1:8765 serving `assets/tone_http.ogg`). The
  expected strings are first guesses: adjust them to what the first green
  run prints when they are right.

### Exactly what is next
1. DONE: run 40135129611's `media / codecs` log read; every compile error of
   the port fixed: `ogg_uint8_t` typedef'd in `compat/media/aosp/include/
   _G_config.h` (Tremolo only defines it on the non-win32 path), `memrchr`
   shimmed in `gesher_media_aosp.h` (OggExtractor), mp3dec takes
   `OSCL_UNUSED_ARG` from the force-included header (CMake silently drops a
   function-like macro given to `target_compile_definitions`),
   `MediaErrors.h` included for `ERROR_UNSUPPORTED` (extractor.cc),
   `-Wno-deprecated-copy` on gesher_media_player (libutils TypeHelpers.h).
2. DONE: linked: `gesher_media_player` into dalvikvm, `EXCLUDE_FROM_ALL`
   dropped everywhere, the codecs job removed from media.yml.
3. Run ci; make `38-media.ps1` green on both runners (the mediaapp run is
   single process, `-Xint`). Then tick the roadmap's MediaPlayer, SoundPool/
   Ringtone/MediaMetadataRetriever and AAudio/OpenSL ES boxes with the run id,
   and add the PASS lines here.
4. DONE: Signal Generator (`org.billthefarmer.siggen` 1.37, versionCode 137)
   added to `tests/e2e/apps.json`, SHA-256
   `e2cad1884047d80856141432169baaf9313722188ef5881e9c2a15d78b1d3601` (the APK
   from the official mirror `https://ftp.gwdg.de/pub/android/fdroid/repo/`;
   f-droid.org itself answers 418 through the local network filter) —
   verified green in e2e before ticking this off.
5. Stage 2: MediaExtractor is written (`media_extractor_jni.cc`); MediaCodec
   and MediaCodecList (audio decoding first: ExoPlayer apps use MediaCodec +
   AudioTrack), MediaMuxer; video needs a Surface to draw into: SurfaceView
   and TextureView are not supported by Gesher's graphics yet (no
   compositor; graphics track), H.264 through Media Foundation (present on
   the CI images). Stage 3: capture (AudioRecord's setup fails cleanly now;
   AAudio input answers UNAVAILABLE; OpenSL recorder FEATURE_UNSUPPORTED).

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
- `aosp.lock`: the new projects above (separate checkouts, no change to
  existing lines).
- `docs/roadmap.md`: the Phase 8 checklist.
