# 0042 — Media: an audio engine per app process, AOSP's extractors and open codecs, Media Foundation for the rest

**Status:** accepted, 2026-10-25 (media track)

## Context
On Android an app's AudioTrack talks to AudioFlinger (audioserver) through
libaudioclient and shared memory; MediaPlayer is a client of mediaserver's
NuPlayer, which runs extractors and codecs in other processes (the media
extractor service, Codec2's software codecs, vendor hardware codecs). None of
those services exist on Windows, and they are large: libaudioclient alone
pulls in the audio HAL's AIDL types, the policy engine and binder services.

Windows has WASAPI for audio output (shared mode mixes every app; each stream
belongs to an audio session the volume mixer shows) and Media Foundation for
codecs. Media Foundation decodes AAC, MP3, FLAC, Opus and H.264 on Windows 10
and 11, and on the CI runners' Windows Server images (`media_probe mf`, ci run
40129230078), but not Vorbis, has no Ogg container, and the Windows "N"
editions lack it unless the Media Feature Pack is installed.

## Decision
1. **Audio output is an engine in each app process**
   (`compat/media/audio`): the framework's AudioTrack natives (and later
   MediaPlayer, SoundPool, AAudio, OpenSL ES) create tracks in it; one thread
   resamples (windowed sinc) and mixes them in float, per Android stream
   volume group, into WASAPI shared mode. No AudioFlinger, no libaudioclient:
   Gesher writes the natives of `android.media.AudioTrack`, `AudioSystem` and
   the audio policy's classes itself; every other native of those classes is
   registered as a zero-answering stand-in found by reflection, so the
   framework's Java code runs unchanged.
2. **Stream volumes are Windows session volumes.** Each stream group (media,
   ringtone, alarm, notifications, calls) in use has its own WASAPI stream in
   its own audio session ("<app> (Alarm)"); its volume is the Android index /
   max, and the volume mixer's changes come back to Android. The volumes live
   in a named mapping all the device's processes share, written by the system
   server's AudioService through AudioSystem's natives as AudioService writes
   AudioFlinger's on a phone.
3. **Machines without an audio endpoint** (CI) get a sink paced by the clock:
   a WAV capture of the mix (`GESHER_AUDIO_CAPTURE`) for tests, or nothing.
   WASAPI failing to open is logged once and retried; it never stops an app.
4. **Containers are AOSP's extractors** (MP3, MPEG-4, Ogg, FLAC, WAV, ADTS,
   later Matroska), built for Windows with their foundation libraries: the
   same track formats (keys, csd buffers, durations, metadata) apps see
   through MediaExtractor and MediaMetadataRetriever on Android.
5. **Royalty-free codecs are AOSP's**, built in: MP3 (mp3dec), Vorbis
   (Tremolo), Opus (libopus), FLAC (libFLAC), PCM and G.711. They work on
   every Windows edition, Server and N included.
6. **Patent-encumbered codecs come from Media Foundation**, whose decoders
   Windows licenses: AAC (and HE-AAC) now, H.264 (and HEVC, VP9, AV1 where
   installed) for video. Gesher does not ship FDK-AAC or H.264 decoders of its
   own. Where Media Foundation is missing, those formats fail with
   MEDIA_ERROR_UNSUPPORTED, as on a device without the codec.

## Consequences
- Every app process mixes its own sound; Windows mixes the processes. The
  latency is WASAPI shared mode's plus a 40 ms buffer.
- AudioFlinger-only features are absent or approximate: effects (AudioEffect,
  Visualizer) are stand-ins; PlaybackParams speed changes pitch too (no time
  stretching); offloaded and direct tracks are refused; VolumeShapers do not
  fade.
- Volumes persist while the device runs (the system server holds the
  mapping), not across restarts of the device.
- The extractors and codecs add to dalvikvm's size; they are static, like the
  rest of the runtime.
