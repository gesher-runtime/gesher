# Media API boundary inventory

This is a source inventory of the current media track, not a claim that the
whole Android media API works. Verified behavior and exact CI runs are in
[media.md](media.md#status-and-next-steps); the open acceptance contracts are
in [media4](handoff/tracks/media4.md) (ADR 0064 reserved).

## Native registration rule

[jni_util.cc](../compat/media/jni/jni_util.cc) first registers every resolvable
native of a class as a zero/false/null stand-in. Each class's explicit
`JNINativeMethod` table then replaces selected entries with implementations.
An unlisted method therefore remains a stand-in; unresolved method types
can prevent even stand-in registration. A method returning zero, or an app
successfully loading a class/library, proves neither that operation's
semantics nor full API compatibility. These source tables are the exact
implemented native-signature inventory for the pinned framework version.

## Implemented subsets and their limits

| API family | Source inventory | Current boundary / remaining contract |
|---|---|---|
| AudioTrack | [audio_track_jni.cc](../compat/media/jni/audio_track_jni.cc), [engine.h](../compat/media/audio/engine.h) | PCM streaming/static tracks, formats/rates, positions/events/volume. No compressed/offload/direct tracks or real effects; underrun incident counts are distinct from lost frames. |
| AudioSystem / AudioManager policy | [audio_system_jni.cc](../compat/media/jni/audio_system_jni.cc), [AudioService.java](../app/java/com/gesher/server/AudioService.java) | Stream indexes/mutes, output/input ports, focus stack, ringer/mode, activity and player/session ids. AudioProductStrategy implements the two legacy-attribute mapping natives; remaining policy natives use stand-ins. This is not Android's full audio policy service. |
| AudioRecord | [audio_record_jni.cc](../compat/media/jni/audio_record_jni.cc), [capture.h](../compat/media/audio/capture.h) | PCM reads, lifecycle, marker/period/timestamp and routed-input reporting; manifest permission gate. `native_setInputDevice` currently always succeeds without changing the capture route. Requested audio-source/input-effect policy and physical routing remain incomplete. |
| AAudio | [aaudio.cc](../compat/media/ndk/aaudio.cc) export table | Input/output PCM, callback and blocking I/O, stream state and positions. Several builder policy/spatialization setters are ignored, input pause/flush return unimplemented and error/disconnection callbacks are not exercised by current CI. API exports are not a complete Android performance/sharing/routing implementation. |
| OpenSL ES | [opensles.cc](../compat/media/ndk/opensles.cc) interfaces/export table | Engine/mix, PCM buffer-queue output, file player and Android simple-buffer-queue recording. The implemented interface tables define the subset; unsupported interfaces/formats must remain explicit. No full OpenSL effects/device-policy engine. |
| MediaPlayer / VideoView | [media_player_jni.cc](../compat/media/jni/media_player_jni.cc), [player.h](../compat/media/player/player.h) | Extract/decode/play, supported data sources, asynchronous prepare, seek/pause/loop/completion and timed video surfaces. Codec availability is machine-dependent; no protected/offloaded/tunneled playback, full playback-rate time stretching or complete native parameter set. |
| SoundPool / Ringtone | [soundpool_jni.cc](../compat/media/jni/soundpool_jni.cc), [media.md](media.md#how-a-file-plays) | Decode whole sounds and mix tracks; synthesized default sound files. Remaining reflected natives do not add effects or a media provider. |
| MediaExtractor | [media_extractor_jni.cc](../compat/media/jni/media_extractor_jni.cc), [extractor.h](../compat/media/player/extractor.h) | AOSP container sniffers/readers, track formats/samples/seeking for supported containers; this does not provide encrypted-sample/DRM processing. |
| MediaMetadataRetriever | [media_jni_others.cc](../compat/media/jni/media_jni_others.cc), `kRetrieverMethods` | Supported path/FD sources, selected container/track tags, album art and frame-at-time/scaling to ARGB_8888. Unlisted natives, full image indexing and BitmapParams/HDR semantics remain outside this implementation. |
| MediaCodec / MediaCodecList | [media_codec_jni.cc](../compat/media/jni/media_codec_jni.cc), `kCodecMethods` / list tables | Decoder/encoder slots, callback/dequeue, format/buffer/Image access, flush/reset and ordinary input/output surfaces. Parameters handle `request-sync` only; scaling-mode setter is empty; metrics returns an empty bundle. Persistent/linear-block, secure/DRM, tunneled/hardware and full dynamic reconfiguration contracts remain absent or unverified. |
| Codec backends | [video_decoder.h](../compat/media/video/video_decoder.h), [encoder.h](../compat/media/encode/encoder.h), [media.md](media.md#video-stage-2-adr-0052) | Software decoders plus available synchronous MF codecs; encoders AAC, Opus and H.264 (MF or libavc). No general hardware, 10-bit output or B-frame encoder path. The software H.264 encoder still needs a forced-soft CI round trip. |
| MediaMuxer | [media_muxer_jni.cc](../compat/media/jni/media_muxer_jni.cc), [muxer.h](../compat/media/mux/muxer.h) | MP4/3GPP and WebM tracks/samples, orientation/location hooks; scoped end-to-end tests cover H.264/AAC MP4 and Opus WebM. Remaining format/error/bounds/timestamp contracts need independent checks. |
| MediaRecorder | [media_recorder_jni.cc](../compat/media/jni/media_recorder_jni.cc), [recorder.h](../compat/media/record/recorder.h) | Audio and ordinary Surface input; Camera2 targets that Surface. `setCamera` logs unsupported; VIDEO_SOURCE_CAMERA is rejected at prepare. Pause/resume, reset/reuse, duration/size notifications and failure paths exist in code but current end-to-end tests do not verify them. Persistent input, next-output-file, audio-routing callbacks and remaining reflected natives are incomplete. |
| MediaSession / MediaController | [MediaSessionService.java](../app/java/com/gesher/server/MediaSessionService.java) | Legacy owner/controller Binder interfaces with metadata/state/queue/extras, commands/callbacks, active tokens and owner cleanup. Unoverridden Default methods and listener permission/user filtering are not complete; Windows transport controls, receiver fallback, Session2 and external volume notifications remain open. |
| Media surfaces / ImageReader bridge | [input_surface.h](../compat/media/video/input_surface.h), [media_codec_jni.cc](../compat/media/jni/media_codec_jni.cc), [surfaces ADR](decisions/0056-surfaces-child-surfaces-and-textures.md) | Media converts ordinary input surfaces to encoder frames and decoder output into ImageReader frames. Shared Surface/SurfaceTexture/EGL composition contracts belong to the surfaces track; its remaining scope is in [surfaces2](handoff/tracks/surfaces2.md). |

## Whole native families still using stand-ins

[media_jni_others.cc](../compat/media/jni/media_jni_others.cc) registers only
reflection stand-ins for MediaSync, MediaHTTPConnection, MediaCrypto,
MediaDrm, MediaDescrambler, MediaProfiles, JetPlayer, ResampleInputStream,
MediaCodec.LinearBlock and MediaCodecInfo.CodecCapabilities. No functional
claim is made for these families; MediaPlayer HTTP reads use Gesher's own
WinHTTP source instead of MediaHTTPConnection.

[media_jni.cc](../compat/media/jni/media_jni.cc) registers only stand-ins for
AudioEffect and Visualizer. [audio_system_jni.cc](../compat/media/jni/audio_system_jni.cc)
does the same for AudioPortEventHandler, AudioVolumeGroup and ToneGenerator.
Session2/media communication is an absent optional framework module, not an
implemented zero stand-in. MediaStore is a separate placeholder with no
media-provider queries/scanning/transcoding; CamcorderProfile lacks the
MediaProfiles backend. Unsupported APIs outside these registered classes
are not made available by this inventory.

## NDK export baseline still open

The machine-readable [native API coverage baseline](handoff/native-api-coverage.json)
records 313 missing exports assigned to media3 from ci 40296960603, on both
runners and both x86_64/arm64: 276 in libmediandk.so, 34 in libOpenSLES.so and
3 in libaaudio.so. These are symbol-presence observations from that exact
run, not semantics or a current-run reassessment.

The AAudio entries are AAudioStream_isMMapUsed, AAudio_getMMapPolicy and
AAudio_setMMapPolicy. OpenSL's entries include unimplemented 3D/effect/device
interface identifiers. libmediandk spans native codec/format/extractor/muxer,
image/reader and protected-media APIs; Java MediaCodec/ImageReader tests do
not establish those NDK exports. media4 inherits this unresolved export
scope and coordinates image/reader implementation with the devices/surfaces
owners. Preserve each baseline observation and add a new run, ABI and
semantic case before claiming an entry implemented. Publishing a zero
function or an interface-id constant alone does not complete its API.

## Evidence and ownership

[38-media.ps1](../tests/windows/tracks/38-media.ps1) and
[59-media3.ps1](../tests/windows/tracks/59-media3.ps1) are the test inventory;
only their actual PASS checks are verified. The runners' file microphone
and test camera do not establish real WASAPI hardware/route/device-removal
behavior. General API coverage requires independent success, error,
lifecycle and permission tests, not simply one sample-app launch.

Follow [media4](handoff/tracks/media4.md) for the unimplemented contracts,
including session permission/user boundaries, NDK recording-denial tests,
recorder lifecycle/limits, physical endpoints and forced software encoding.
Keep the source table links current when replacing a stand-in. Broader
native ABI/API auditing is owned by native3; this inventory does not replace
its cross-framework baseline.
