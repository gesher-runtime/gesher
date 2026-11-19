# Capture availability and device loss

Normal application capture uses the actual Windows input endpoint. A missing,
unsupported or unstartable endpoint is an error; hardware failure never selects
silence, a file or loopback as a replacement. A WASAPI packet explicitly marked
`AUDCLNT_BUFFERFLAGS_SILENT` remains genuine captured silence. A recoverable
`GetBuffer` `AUDCLNT_E_BUFFER_ERROR` waits for the next processing pass without
creating samples ([Microsoft contract](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer)).
When the shared source restarts after becoming idle, it resets the stopped
WASAPI stream before starting, flushing queued packets from the previous
activation. A failed Reset or Start reports unavailable instead of admitting
capture. Starting an already running source does not reset it: another active
recorder's input remains continuous. This follows Windows'
[Stop/resume](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-stop)
and [Reset/flush](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-reset)
contracts.

The engine opens and starts its COM-owned source on its worker. A synchronous
command acknowledges initialization/start before the caller reports success.
Stream generations invalidate an admission overtaken by Stop or Release; no
stream lock is held while waiting for the engine. Capture-worker callbacks may
start another stream without waiting on their own worker. Backend failure wakes
readers and retires started streams instead of producing synthetic samples.
Each publication also carries the target stream's captured generation, so a
callback that restarts a peer cannot deliver that peer an older batch. Wake
only posts an atomic idle recheck: dropping the final weak-lock owner under
the engine mutex cannot recursively lock the engine through stream destruction.

The AAudio input worker observes a capture-owned atomic wake sequence. It
snapshots that sequence before checking the stream state and terminal error,
then waits without either stream mutex held. Fail, Stop and Release advance
and notify the sequence, as do AAudio start and disconnect transitions. A
failure delivered after Stop's error check therefore wakes an inactive monitor;
notification between the predicate check and wait also cannot be lost. The
AAudio stream retains its capture object until the worker joins. No API
callback or API-mutex acquisition runs on the engine's failure path, and an
inactive or error-only monitor does not poll for device loss.

| Android API | Initialization/start failure | Device loss |
| --- | --- | --- |
| AudioRecord | Existing native setup failure (`-20`); failed native start leaves recording unstarted | A read first returns genuine bytes already transferred, then `ERROR_DEAD_OBJECT` (`-6`); zero-length reads remain zero |
| AAudio input | `AAUDIO_ERROR_UNAVAILABLE` for unavailable capture; failed start preserves the previous stream state | A separate terminal flag exposes `DISCONNECTED`; an asynchronous failure invokes one registered error callback on the owned input worker, outside stream locks. A synchronous read or active stop consumes pending disconnection without an error callback. Partial synchronous reads return their transferred frame count first. |
| OpenSL recorder | Creation stores configuration; `Realize` returns `SL_RESULT_CONTENT_UNSUPPORTED` if capture cannot initialize and leaves the object unrealized | Queue filling stops without fabricated samples or buffer-completion callbacks; SetRecordState preserves Android's requested-state acknowledgment, and an unstartable capture cannot admit filling |
| MediaRecorder | Prepare fails when capture cannot initialize; failed Start admits no muxer/recording thread, and muxer startup failure stops capture | The audio worker reports `MEDIA_RECORDER_ERROR_UNKNOWN`, stops feeding and wakes peer workers; Stop cleans up and reports failure |

Contract evidence:

- [AudioRecord read/error semantics](https://developer.android.com/reference/android/media/AudioRecord)
  and [AOSP initialization status](https://android.googlesource.com/platform/frameworks/av/+/fd6444c51fb985d66ca9aae491759ce750bb173a/include/media/AudioRecord.h).
- [AAudio disconnected streams and callback restrictions](https://developer.android.com/ndk/guides/audio/aaudio/aaudio#disconnected_audio_stream).
  Android16 r4 [AudioStreamRecord](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libaaudio/src/legacy/AudioStreamRecord.cpp)
  acknowledges a pending disconnect when stopping without calling the error
  callback, and records a synchronous read's `DEAD_OBJECT` before returning.
  [AudioStream](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libaaudio/src/core/AudioStream.cpp)
  retains disconnection separately from internal lifecycle: a known disconnected
  start returns `AAUDIO_ERROR_INVALID_STATE`, while valid/redundant stops retain
  their lifecycle behavior and the external state stays `DISCONNECTED`.
  [AudioStreamLegacy::onAudioDeviceUpdate](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libaaudio/src/legacy/AudioStreamLegacy.cpp)
  calls `forceDisconnect()` with error notification when a new device update
  arrives while the data callback is inactive. Only a pending disconnect
  already consumed by stop or synchronous read suppresses that notification;
  becoming stopped does not discard a genuinely late failure.
- Android 16 [OpenSL recorder realization/start backend](https://android.googlesource.com/platform/frameworks/wilhelm/+/android-16.0.0_r1/src/android/AudioRecorder_to_android.cpp)
  maps unsuccessful `AudioRecord::initCheck()` to `CONTENT_UNSUPPORTED`.
  [IRecord](https://android.googlesource.com/platform/frameworks/wilhelm/+/android-16.0.0_r1/src/itf/IRecord.cpp)
  acknowledges a valid requested recording state independently of backend start's
  result. No extra OpenSL resource-loss callback is invented here.
  [Descriptor validation](https://android.googlesource.com/platform/frameworks/wilhelm/+/android-16.0.0_r1/src/data.cpp)
  remains at Create, including channel/rate bounds, PCM container and
  representation, mask count, native byte order, queue size and default input.
- [MediaRecorder errors](https://developer.android.com/reference/android/media/MediaRecorder).

`GESHER_CAPTURE_TEST_SOURCES` defaults OFF. Only isolated fixture binaries built
with this option accept the `GESHER_AUDIO_SOURCE` WAV/null/loopback choices. A
failed explicit fixture also fails honestly. CI seals normal and fixture host
roles separately, checks their differing capture-only compile definitions and
the fixture-only sentinel, restores production configuration, and keeps fixture
executables out of the ordinary runtime and release installer.

Validation:

- `tools/test_capture_failures.py --cxx <llvm-mingw-clang++>` compiles the whole
  capture worker/stream with native Windows clocks and synchronization. WASAPI
  and thread priority are controlled boundaries. It tests missing input, start
  failure, disconnect, partial/empty reads, deterministic Stop/Release races,
  callback reentrancy and production/fixture separation. It also executes
  production OpenSL recorder entry-point bodies, MediaRecorder Start/AudioLoop
  and AudioRecord read translation. Unrelated object allocation, encoding,
  muxing and JNI receiver lookup remain declared fixture boundaries.
  The same driver compiles the entire WASAPI implementation with an injected
  `IAudioCaptureClient`/`IAudioClient` boundaries and checks empty buffers, recoverable errors,
  device/service failure, genuine silent packets, exact float samples and
  invalid null payloads. A queued pre-stop packet must disappear on restart;
  failed Reset never calls Start, successful Reset still propagates Start
  failure, and repeated Start/Stop preserves the current source state. No COM
  audio device is opened by these fixtures.
- `tools/test_aaudio_lifecycle_contracts.py` compiles all AAudio functions and
  exports with controlled Track/Capture observers, including partial input reads
  and error-only callback delivery. Deterministic callback and monitor barriers
  distinguish an error already pending at Stop from one delivered afterward,
  both with an in-flight data callback and with an idle error-only monitor.
  Builder and existing PCM/callback/ring tests remain separate checks; the
  builder extractor includes the actual disconnect helper bodies and starts
  callback streams through the production lifecycle.
- Local production and fixture `media_probe` targets were built with real D:
  AOSP/toolchain inputs. Production rejected a synthetic override. The fixture
  captured a generated WAV for approximately two seconds; no physical
  microphone was recorded. This is not full ART, both guest ABIs, unplugged
  hardware or release-installer validation; Windows CI retains those applicable
  production/runtime gates.
