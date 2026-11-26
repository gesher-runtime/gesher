# Text-to-speech: Android's TextToSpeech over Windows voices

Android apps speak through `android.speech.tts.TextToSpeech`, which binds the engine
package's `TextToSpeechService` and receives the audio of each utterance through it.
Gesher had no engine: `new TextToSpeech(context, listener)` found no
`android.intent.action.TTS_SERVICE` service and reported `onInit(ERROR)`. Gesher now
ships one, **Gesher Text-to-speech** (`com.gesher.tts`, `app/gesher-tts`), whose voices
are the PC's Windows voices and whose audio is rendered by Windows.

## The Android contract (android-16.0.0_r4)

Sources: `frameworks/base` at `android-16.0.0_r4` (GitHub mirror
`msft-mirror-aosp/platform.frameworks.base`), `core/java/android/speech/tts/`.

- `TextToSpeech.initTts` connects to the requested engine, else the user's default
  (`TtsEngines.getDefaultEngine`: Settings.Secure `tts_default_synth` "if the engine is
  available on the device", else `getHighestRankedEngineName`, which only answers an
  engine with `ApplicationInfo.FLAG_SYSTEM`), else that highest-ranked system engine.
  Ordinary apps connect with `DirectConnection` (`bindService` of the engine's
  `TTS_SERVICE` intent); only the system-only constructor uses `SystemConnection` and
  the `texttospeech` manager service, so Gesher needs no such service.
- `TtsEngines.getEngines()` is `queryIntentServices(TTS_SERVICE, MATCH_DEFAULT_ONLY)`.
  Apps targeting API 30+ see engines only with a `<queries>` element for that intent;
  Gesher's package visibility applies, as on Android.
- `TextToSpeechService` (the framework's own class, unchanged) owns the request queue,
  audio playback, `synthesizeToFile` (`FileSynthesisCallback` writes the WAV header from
  `start()`'s format at `done()`) and the client's `UtteranceProgressListener` events.
  The engine implements `onIsLanguageAvailable`, `onLoadLanguage`, `onGetLanguage`,
  `onGetVoices`, `onIsValidVoiceName`, `onLoadVoice`, `onGetDefaultVoiceNameFor`,
  `onGetFeaturesForLanguage`, `onStop` and `onSynthesizeText`.
- `SynthesisCallback`: "`start`, then `audioAvailable` until all audio has been
  provided, then finally `done`"; "`error` can be called at any stage"; "`done` must be
  called at the end of synthesis, regardless of errors"; `audioAvailable` length "must be
  less than or equal to the return value of `getMaxBufferSize`"; `rangeStart(markerInFrames,
  start, end)` with `end` "exclusive", delivered to the client's `onRangeStart` "at the
  moment that frame has been reached by the playback head".
- `ACTION_CHECK_TTS_DATA`: `CHECK_VOICE_DATA_PASS`/`FAIL`, `EXTRA_AVAILABLE_VOICES` and
  `EXTRA_UNAVAILABLE_VOICES` as `lang-COUNTRY-variant` in ISO 639-2/3166 alpha-3
  ("eng-USA"); `EXTRA_UNAVAILABLE_VOICES` lists "ones that user can install".

## The Windows side

`Windows.Media.SpeechSynthesis.SpeechSynthesizer` (MicrosoftDocs/winrt-api), through
its raw WinRT ABI from plain C++ (`compat/devices/speech_win.cc`, as
`location_win.cc`): interface IIDs and method order are those of Windows' metadata
(as microsoft/windows-rs generates them), combase and shcore are found at run time.

- `SpeechSynthesizer.AllVoices` / `DefaultVoice`: each `VoiceInformation` has `Id`,
  `DisplayName`, `Language` ("the normalized BCP-47 language tag") and `Gender`. "Only
  Microsoft-signed voices installed on the system can be used."
- `SynthesizeTextToStreamAsync(text)`: a `SpeechSynthesisStream` holding a RIFF WAV;
  read through `CreateStreamOverRandomAccessStream`. Canceled through `IAsyncInfo`.
- `SpeechSynthesizerOptions.SpeakingRate` ("0.5 ... to 6.0 ..., default 1.0") and
  `AudioPitch` ("0.0 ... to 2.0, inclusive. The default value is 1.0"), Windows 10 1709.
- `IncludeWordBoundaryMetadata`: the stream's `TimedMetadataTracks` carry a
  `SpeechWord` track of `SpeechCue`s, with `StartTime`, `StartPositionInInput` ("the
  first character in the word") and `EndPositionInInput` ("the last character").

## The engine

- **Install and default.** At its start the device installs `gesher-tts.apk` (next to
  dalvikvm.exe) when Windows has at least one voice, and uninstalls it when there are
  none (`TtsEngineApp`, as `CameraApp` does for cameras). When it is installed,
  Settings.Secure `tts_default_synth` gets `com.gesher.tts` as a default row: on Android
  the system image's engine is chosen through `FLAG_SYSTEM`, which this device-installed
  package does not carry, so the setting names it; a value written to the setting is
  kept. `tts_default_locale` is not set: the engine then follows the device locale, as
  `TtsEngines.getLocalePrefForEngine` does without a preference.
- **Voices.** Exactly the Windows voices. Each gets a stable Android name: its language
  tag and a private-use subtag from its Windows name (`he-IL-x-asaf`,
  `en-US-x-zira`), a `Voice` with that locale, `QUALITY_NORMAL`, `LATENCY_NORMAL`, no
  network connection and the feature `embeddedTts` only (`networkTts` is never
  offered). Languages: `LANG_COUNTRY_AVAILABLE` when a voice has the language and
  country, `LANG_AVAILABLE` for the language alone, otherwise `LANG_NOT_SUPPORTED`;
  Windows voices have no variants, so `LANG_COUNTRY_VAR_AVAILABLE` is never answered.
  A locale's default voice is Windows' default voice if it speaks it, else one of the
  device's country, else the first by name.
- **Synthesis.** The request's voice (or its locale's default voice; none:
  `error(ERROR_INVALID_REQUEST)`), its text as plain text, Android's rate and pitch
  (percent of normal) as Windows factors clamped to Windows' ranges. Windows' PCM is
  handed over unchanged: `start(sampleRate, ENCODING_PCM_16BIT, channels)` (8-bit and
  float PCM pass as such; other formats are `ERROR_SYNTHESIS`), each word's
  `rangeStart(frame, first, last + 1)` before the audio containing it, `audioAvailable`
  in whole frames of at most `getMaxBufferSize()`, `done()`. Failures are
  `error(ERROR_SYNTHESIS)` then `done()`; a rate or pitch Windows cannot apply (before
  10 1709) is `ERROR_INVALID_REQUEST`, never ignored.
- **Stop.** `onStop` cancels the running Windows synthesis; the framework has already
  stopped the callback, so the client hears `onStop` and no audio follows.
- **CHECK_TTS_DATA.** `CheckVoiceDataActivity` answers without UI: PASS with the
  installed voices' languages (narrowed to `EXTRA_CHECK_VOICE_DATA_FOR`), FAIL without.
  `EXTRA_UNAVAILABLE_VOICES` is empty and there is no `INSTALL_TTS_DATA` activity:
  Windows installs its voices (Settings, Time & language, Speech).

## Limits

- Voices installed in Windows while the engine runs appear when it next lists voices
  (`getVoices`); a voice removed meanwhile fails one request and is then dropped.
  The engine package itself is installed or removed at the device's next start.
- `TtsSpan`s are not interpreted (the text is spoken as plain text); SSML is not used.
- Desktop (SAPI 5) voices that are not OneCore voices are not in `AllVoices` and are not
  offered.

## Tests

- `tools/test_tts_engine_contracts.py`: the production engine sources on a host JVM
  with an isolated test backend and stand-ins for the framework's callbacks
  (Linux and Windows CI).
- `tests/tts/tts_windows_test.cc`: `speech_win` against real Windows voices, no UI
  (windows-host-contracts).
- `tests/tts/tts_probe.cc`: a by-hand probe that writes a Hebrew (else en-US) utterance
  to a WAV file.
