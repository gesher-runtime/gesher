# Speech recognition over Windows' recognizers

Android apps recognize speech with `SpeechRecognizer`, which binds a
`RecognitionService` (upstream #157, `SpeechRecognitionManagerService`; see
[devices.md](devices.md#speech-recognition)). Gesher ships one such service, **Gesher
Speech Recognition** (`app/gesher-stt`, package `com.gesher.stt`). Windows' SAPI 5
recognizers do the recognizing. The audio is captured by Android's `AudioRecord`, which is
Gesher's microphone path, on behalf of the calling app.

```
app ── SpeechRecognizer ──Binder──> system server: SpeechRecognitionManagerService (#157)
                                         └─ binds com.gesher.stt/.WindowsRecognitionService
                                              ├─ RecognitionService (AOSP): busy, RECORD_AUDIO
                                              ├─ AudioRecord (attribution context for the app)
                                              └─ WindowsSpeechRecognition (boot class path, JNI)
                                                   └─ compat/devices/speech_recognition_win.cc
                                                        SAPI 5 in-process recognizer (dictation)
```

## Why SAPI 5 and not Windows.Media.SpeechRecognition

Microsoft lists every `Windows.Media.SpeechRecognition` class among the WinRT APIs that
need package identity. In windows-dev-docs `hub/apps/desktop/modernize/winrt-api-desktop-app-support.md`:

> The following WinRT classes require package identity ... These APIs are supported only
> in desktop apps that are packaged (that is, that have package identity at runtime).
>
> * Windows.Media.SpeechRecognition.SpeechContinuousRecognitionSession
> * ...
> * Windows.Media.SpeechRecognition.SpeechRecognizer

Gesher's processes (`dalvikvm.exe`, installed by Inno Setup) have no package identity.
Its dictation also runs as a web service. `speech-recognition.md` in windows-dev-docs
says: "When using these grammars, speech recognition is performed by a remote web
service", and dictation requires the "Online speech recognition" privacy setting
(`HResultPrivacyStatementDeclined = 0x80045509`). Gesher does not work around the identity
requirement. It uses SAPI 5's in-process recognizer (`CLSID_SpInprocRecognizer`), a COM
API for desktop processes. SAPI recognizes on the PC. It needs neither the online setting
nor a network.

SAPI only sees the recognizers registered under `SPCAT_RECOGNIZERS`
(`HKLM\SOFTWARE\Microsoft\Speech\Recognizers`). Each recognizer lists the LCIDs it
recognizes in its `Language` attribute. Gesher offers exactly those languages. A Windows
installation may have none. The OneCore speech languages used by the WinRT API are a
separate set and are not offered. **Hebrew is offered only if a SAPI recognizer lists
it.**

## Android contract and Gesher's answers

AOSP `android-16.0.0_r4`: [`RecognitionService`](https://github.com/msft-mirror-aosp/platform.frameworks.base/blob/android-16.0.0_r4/core/java/android/speech/RecognitionService.java),
[`SpeechRecognizer`](https://github.com/msft-mirror-aosp/platform.frameworks.base/blob/android-16.0.0_r4/core/java/android/speech/SpeechRecognizer.java),
[`RecognizerIntent`](https://github.com/msft-mirror-aosp/platform.frameworks.base/blob/android-16.0.0_r4/core/java/android/speech/RecognizerIntent.java),
[`RecognitionSupport`](https://github.com/msft-mirror-aosp/platform.frameworks.base/blob/android-16.0.0_r4/core/java/android/speech/RecognitionSupport.java),
[`DefaultPermissionGrantPolicy`](https://github.com/msft-mirror-aosp/platform.frameworks.base/blob/android-16.0.0_r4/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java).

| What the app does | Windows has a recognizer for the language | Otherwise |
| --- | --- | --- |
| `isRecognitionAvailable` | `true`: the device installs `com.gesher.stt` at start | `false`: no recognizer, no service installed |
| `createSpeechRecognizer(ctx)` | binds `com.gesher.stt` (`voice_recognition_service` defaults to it) | the manager's existing errors |
| `startListening` without `RECORD_AUDIO` | `ERROR_INSUFFICIENT_PERMISSIONS`, from AOSP's `RecognitionService`. No microphone is opened | same |
| `startListening` | `onReadyForSpeech`, `onBeginningOfSpeech` (SAPI `SPEI_SOUND_START`), `onRmsChanged` (dBFS of the captured PCM), `onPartialResults` (`SPEI_HYPOTHESIS`, only with `EXTRA_PARTIAL_RESULTS`), `onEndOfSpeech`, `onResults` (`RESULTS_RECOGNITION`: the result and its alternates, up to `EXTRA_MAX_RESULTS`, default 5; `CONFIDENCE_SCORES`: `SREngineConfidence` when in [0, 1], else -1) | `ERROR_LANGUAGE_NOT_SUPPORTED` |
| `EXTRA_LANGUAGE` | an exact tag, or a bare language (`en`) served by a recognizer of that language (the default recognizer's first). No other region is substituted | `ERROR_LANGUAGE_NOT_SUPPORTED`. A recognizer whose engine fails to load gives `ERROR_LANGUAGE_UNAVAILABLE` |
| no `EXTRA_LANGUAGE` | `Locale.getDefault()`, as `RecognizerIntent` documents | same |
| sound without a match | `ERROR_NO_MATCH` (`SPEI_FALSE_RECOGNITION`, or speech heard but nothing recognized) | |
| no speech for 5 s | `ERROR_SPEECH_TIMEOUT` | |
| `stopListening` | capture ends at once. SAPI recognizes the audio it already has, then `onResults` or an error | `ERROR_CLIENT` without a session (AOSP) |
| `cancel`, client death, `onDestroy` | no more callbacks. The SAPI session and `AudioRecord` are released | |
| second concurrent session | `ERROR_RECOGNIZER_BUSY` (AOSP's default of one session) | |
| SAPI failure (setup or mid-stream HRESULT) | `ERROR_SERVER` | |
| capture failure (`AudioRecord` refused, not initialized, read error) | `ERROR_AUDIO` | |
| `ERROR_NETWORK*` | never: SAPI recognition makes no network request | |
| `EXTRA_AUDIO_SOURCE` | the app's 16-bit mono PCM, 8 to 48 kHz, read until closed. Other encodings or channel counts give `ERROR_AUDIO` | |
| `EXTRA_PREFER_OFFLINE` | already satisfied: every recognition runs on the PC | |
| `checkRecognitionSupport` | `installedOnDeviceLanguages` = Windows' recognizer languages; the other lists are empty | empty lists |
| `triggerModelDownload` (listener form) | `onSuccess` for an installed language | `ERROR_LANGUAGE_NOT_SUPPORTED`. Gesher cannot download a SAPI recognizer |
| `createOnDeviceSpeechRecognizer` / `isOnDeviceRecognitionAvailable` | unchanged: `config_defaultOnDeviceSpeechRecognitionService` is empty in framework-res | unchanged |
| `ACTION_RECOGNIZE_SPEECH` activity | none yet (`ActivityNotFoundException`, as before) | |

### Permissions

AOSP's `RecognitionService.dispatchStartListening` calls `onStartListening` before
`checkPermissionAndStartDataDelivery`. If the check fails, it reports
`ERROR_INSUFFICIENT_PERMISSIONS` and calls `onCancel`. That check is skipped when the
service already created an attribution context for the caller. `WindowsRecognitionService`
therefore opens nothing in `onStartListening`. The session starts from the main handler
after the check, and the attribution context is created only when `AudioRecord` opens.
Gesher's `AudioRecord` checks the recording process's own `RECORD_AUDIO`
(`AudioService.recordAudioAllowed`). `SttEngineApp` grants the engine `RECORD_AUDIO` on
its first install. AOSP's `DefaultPermissionGrantPolicy` does the same for the system's
voice recognition package:

```java
// Voice recognition
Intent voiceRecoIntent = new Intent(RecognitionService.SERVICE_INTERFACE)
        .addCategory(Intent.CATEGORY_DEFAULT);
grantPermissionsToSystemPackage(pm,
        getDefaultSystemHandlerServicePackage(pm, voiceRecoIntent, userId), userId,
        MICROPHONE_PERMISSIONS);
```

The grant is not repeated for an update, so a user's later revocation is kept.

## Windows details

- Recognizers: `ISpObjectTokenCategory` on `SPCAT_RECOGNIZERS`, `EnumTokens`, attribute
  `Language` (hexadecimal LCIDs, `;`-separated) mapped with `LCIDToLocaleName`. The default
  recognizer is `GetDefaultTokenId`.
- Session: `CLSID_SpInprocRecognizer`, `SetRecognizer(token)`, `SetInput(SpStream)`. The
  stream is an `IStream` over the PCM that `AudioRecord` captured, declared as
  `SPDFID_WaveFormatEx` (16-bit mono at the capture rate; `fAllowFormatChanges` lets SAPI
  convert). Then `CreateRecoContext`, `SetNotifyWin32Event`, `SetInterest`,
  `SetMaxAlternates`, `CreateGrammar`, `LoadDictation(NULL, SPLO_STATIC)`,
  `SetDictationState(SPRS_ACTIVE)` and `SetRecoState(SPRST_ACTIVE)`.
- One thread per session creates, drives and releases every SAPI object (MTA). Events are
  queued. The engine's thread reads them, so no native thread calls into Java.
- `SPDFID_WaveFormatEx` is only declared in `sapi.h` (defined in `sapi.lib`). Its value is
  Microsoft's `SpeechAudioFormatGUIDWave` in the same header (`sapi53.h`, as recompiled in
  microsoft/win32metadata):
  `const BSTR SpeechAudioFormatGUIDWave = L"{C31ADBAE-527F-4ff5-A230-F62BB61FF70C}";`.
  Only `ole32` and `uuid` are linked, and `sapi.dll` is reached through COM.

## Tests

- `tools/test_speech_recognition_engine_contracts.py`: Java host contracts. They compile the
  unchanged engine and `WindowsSpeechRecognition` against stand-ins whose
  `RecognitionService` follows AOSP's dispatch. `--source-ref upstream/main` fails, because
  the sources do not exist there.
- `tests/speech/speech_recognition_windows_test.cc`: Windows host test without a
  microphone or UI (CI `windows-host-contracts`, MSVC `/W4 /WX`). It covers recognizer
  enumeration, language matching, and an unsupported language failing with
  `SPERR_NOT_FOUND`. When the machine has a recognizer, it also checks that silence handed
  over as PCM ends the stream without a recognition, and that cancel is prompt.
- `tests/speech/recognize_probe.cc` + `tests/speech/HUMAN_TEST.md`: 5 s on a real
  microphone.

## Not covered yet

- An `ACTION_RECOGNIZE_SPEECH` activity. It needs UI, which the project owner reviews from
  screenshots.
- The on-device factory (`createOnDeviceSpeechRecognizer`). It would need a framework-res
  overlay that names a package installed only when Windows has recognizers.
- Segmented sessions (`EXTRA_SEGMENTED_SESSION`), biasing strings and language detection.
  These extras are optional, and the service ignores them.
