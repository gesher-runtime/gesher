# Human test on Windows 11: Gesher speech recognition over SAPI 5

Branch `feat/speech-recognition-windows` of `https://github.com/dudua99/gesher`. Steps 1 to 4
show no UI. Step 2 listens on the microphone for 5 seconds, and nothing plays sound.
Gesher adds no dialog in this branch, so there is nothing to screenshot.

## 0. Prerequisites and what to check first

- Windows 11 Pro build 26300, MSVC 2022 ("x64 Native Tools Command Prompt for VS 2022"),
  Git, Python 3 and a JDK 17 or newer (for step 4).
- A microphone that works in Settings, System, Sound, Input ("Test your microphone").
- Settings, Privacy & security, Microphone: "Microphone access" on, and "Let desktop apps
  access your microphone" on. The probe in step 2 and Gesher are desktop apps.
- **Which SAPI recognizers this PC has.** This decides everything else. In PowerShell:

  ```powershell
  Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Speech\Recognizers\Tokens' -ErrorAction SilentlyContinue |
    ForEach-Object { '{0}  Language={1}' -f (Get-ItemProperty $_.PSPath).'(default)', (Get-ItemProperty (Join-Path $_.PSPath Attributes)).Language }
  ```

  Expected: zero or more lines such as
  `Microsoft Speech Recognizer 8.0 for Windows (English - US)  Language=409;9`. **Please
  report the exact output.** Nobody has checked yet whether build 26300 still ships the
  desktop (SAPI) recognizers that Windows Speech Recognition used. If the list is empty,
  Gesher correctly installs no recognition service (step 5.1). The OneCore speech
  languages under `HKLM:\SOFTWARE\Microsoft\Speech_OneCore\Recognizers` belong to the
  WinRT API. They do not count, and Gesher does not use them (docs/speech-recognition.md).
  A Hebrew line is expected only if Windows really has a Hebrew SAPI recognizer.

## 1. Build the host test and the probe

```bat
git clone https://github.com/dudua99/gesher gesher-stt
cd gesher-stt
git checkout feat/speech-recognition-windows

cl /nologo /std:c++17 /EHsc /W4 /WX /D_WIN32_WINNT=0x0A00 /Icompat\devices tests\speech\speech_recognition_windows_test.cc compat\devices\speech_recognition_win.cc /Fe:speech-recognition-windows.exe ole32.lib uuid.lib

cl /nologo /std:c++17 /EHsc /W4 /WX /D_WIN32_WINNT=0x0A00 /DUNICODE /D_UNICODE /Icompat\devices tests\speech\recognize_probe.cc compat\devices\speech_recognition_win.cc /Fe:recognize-probe.exe ole32.lib uuid.lib winmm.lib
```

Expected: both compile without warnings and produce the two `.exe` files.

## 2. Probe: listen 5 seconds, print hypotheses and the result

```bat
recognize-probe.exe
```

When it prints `listening for 5 seconds; speak now`, say clearly, in the default
recognizer's language: "hello world this is a test of speech recognition".

Expected output. The shape matters; names, texts and numbers vary:

```
recognizers: HRESULT 0x00000000, 1
  Microsoft Speech Recognizer 8.0 for Windows (English - US) [en-US,en] (default)
session en-US: HRESULT 0x00000000
listening for 5 seconds; speak now
[sound start]
[hypothesis] "hello" (confidence -1.00)
[hypothesis] "hello world this is" (confidence -1.00)
...
[sound end]
[RESULT] "hello world this is a test of speech recognition" (confidence 0.87) "..." (confidence ...)
stopped listening; recognizing the rest
[stream end]
```

- `[RESULT]` may come before or after `stopped listening`.
- Confidence values are printed as Windows reports them; `-1.00` means Windows reported
  none in [0, 1].
- Silence for the whole 5 s: `[stream end]` with no `[RESULT]`, possibly after a
  `[no match]`. Gesher then reports `ERROR_SPEECH_TIMEOUT` or `ERROR_NO_MATCH`.
- `recognizers: ..., 0` followed by `no SAPI recognizer` (exit code 2): the PC has no SAPI
  recognizer. Report it; this is the expected result on such a PC.
- `waveInOpen failed`: the microphone is missing, or blocked for desktop apps (step 0).

More runs:

```bat
recognize-probe.exe en-US
recognize-probe.exe he-IL
recognize-probe.exe xx-XX
```

Expected: `en-US` behaves as above (if there is an en-US recognizer). If no SAPI
recognizer lists Hebrew, `he-IL` prints `session he-IL: HRESULT 0x8004503A` and exits 1;
`SPERR_NOT_FOUND` is what Gesher reports as `ERROR_LANGUAGE_NOT_SUPPORTED`. `xx-XX` prints
the same `0x8004503A`.

**Please report back:** the full output of each run, and whether the `[RESULT]` text
matches what you said.

## 3. Windows host test (the same test CI runs)

```bat
speech-recognition-windows.exe
echo %ERRORLEVEL%
```

Expected: only `PASS` lines (no `FAIL`), then `0 failure(s)` and `0`. With no SAPI
recognizer, it prints `SKIP recognition: this machine has no SAPI recognizer` and still
ends with `0 failure(s)`. With a recognizer, the silence session must end
(`the stream ends after EndAudio`) in under about 20 s.

## 4. Java service contracts (any OS with JDK 17+ and Python 3)

```bat
python tools\test_speech_recognition_engine_contracts.py
python tools\test_speech_recognition_engine_contracts.py --source-ref upstream/main
```

Before the second command, run `git remote add upstream https://github.com/gesher-runtime/gesher`
and `git fetch upstream main`.

Expected: the first prints `64 checks, 0 failure(s)` and exits 0. The second fails with
`git show upstream/main:app/gesher-stt/...` (exit 1) because the service does not exist
there. That failure is the negative control.

## 5. End to end on the Gesher device (needs the full WSL llvm-mingw build of this branch)

Build Gesher from this branch as usual in WSL, including the framework step that builds
`gesher-stt.apk` (`.github/workflows/framework.yml`, the `APP_DIR="$PWD/app/gesher-stt"`
line). Put `gesher-stt.apk` next to `dalvikvm.exe`, then install or run the build.

1. Start the device (start any app, or run `gesher-app.cmd list`). Then open
   `%LOCALAPPDATA%\Gesher\logs\system_server.log` and look for one of:
   - `speech recognition: com.gesher.stt installed (N Windows recognizer(s))`, or
     `unchanged` on later starts, when step 0 listed recognizers;
   - `speech recognition: no Windows recognizer, none installed` when it listed none.
     That is the correct Android answer: `SpeechRecognizer.isRecognitionAvailable` is false.
2. With recognizers: `gesher-app.cmd list` lists `com.gesher.stt`.
3. Install an app that uses `android.speech.SpeechRecognizer`, for example a notes or
   dictation app with a microphone button that does not need Google services. Grant it the
   microphone when asked. Tap its microphone and say the sentence from step 2. Expected: the
   text appears in the app. The app's log (`gesher-app.cmd logcat`, if available, or the
   app's own log in `%LOCALAPPDATA%\Gesher\logs`) shows no `GesherSpeech` warnings.
4. Deny the app's microphone permission (Gesher's app settings), then tap the microphone
   again. Expected: the app reports a permission error (`ERROR_INSUFFICIENT_PERMISSIONS`,
   9), and the Windows microphone indicator in the taskbar does **not** light up.
5. Ask in a language Windows has no recognizer for. Many apps follow the device language;
   set Gesher's language to Hebrew if step 0 listed no Hebrew. Expected: the app reports
   "language not supported" (`ERROR_LANGUAGE_NOT_SUPPORTED`, 12). Nothing is recognized as
   another language.

**Please report back:** the log lines from 5.1, the app you used, and the observed results
of 5.3 to 5.5.

## What this run verifies, and what it does not

It verifies the real SAPI recognizers, the real microphone (probe), the real MSVC build,
and, with step 5, the whole Android path on the device. It does not verify
`EXTRA_AUDIO_SOURCE` from a real app, other SAPI engines (third-party recognizers), or
ARM64 Windows.
