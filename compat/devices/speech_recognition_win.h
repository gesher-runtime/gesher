// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Gesher (devices track, docs/speech-recognition.md): Windows' speech recognition
// for Android's RecognitionService, over SAPI 5's in-process recognizer
// (CLSID_SpInprocRecognizer, sapi.dll).
//
// Windows.Media.SpeechRecognition.SpeechRecognizer is listed by Microsoft among the
// WinRT classes that require package identity ("Windows Runtime APIs not supported
// in desktop apps", windows-dev-docs hub/apps/desktop/modernize/
// winrt-api-desktop-app-support.md). Gesher's processes are unpackaged, so the
// recognizers used here are SAPI's: the tokens of SPCAT_RECOGNIZERS
// (HKLM\SOFTWARE\Microsoft\Speech\Recognizers), each with the LCIDs it recognizes.
//
// Audio is not captured here. The caller hands over 16-bit mono PCM that Android's
// AudioRecord captured (Gesher's microphone path, with the app's permission and
// attribution); a session's stream feeds it to the recognizer as SPDFID_WaveFormatEx.
//
// No callbacks: the recognizer's events are queued and read with NextEvent, from a
// thread the caller owns (the JNI caller's own Java thread).

#ifndef GESHER_COMPAT_DEVICES_SPEECH_RECOGNITION_WIN_H_
#define GESHER_COMPAT_DEVICES_SPEECH_RECOGNITION_WIN_H_

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gesher::devices::speech_recognition {

// SAPI's own HRESULTs that this file reports (sperror.h).
constexpr HRESULT kSpErrNotFound = static_cast<HRESULT>(0x8004503AL);  // SPERR_NOT_FOUND

// Keep the native allocation bounded even when a caller bypasses the Java limit.
// RecognizerIntent.EXTRA_MAX_RESULTS is a maximum; fewer real alternatives are valid.
// Keep in sync with WindowsSpeechRecognition.MAX_ALTERNATES.
constexpr int kMaxAlternates = 64;

// One installed SAPI recognizer token.
struct Recognizer {
  std::wstring token_id;
  std::wstring name;
  // The token's "Language" attribute as BCP-47 tags (LCIDToLocaleName), in the
  // token's order. Neutral LCIDs a token also lists (such as 9 for English) are
  // kept as their neutral tag ("en").
  std::vector<std::wstring> languages;
  // SPCAT_RECOGNIZERS' default token (Control Panel, Speech Recognition).
  bool is_default = false;
};

// The installed recognizers. S_OK with an empty list when Windows has none;
// a failing HRESULT when SAPI cannot be used at all (no sapi.dll, COM failure).
HRESULT EnumerateRecognizers(std::vector<Recognizer>* out);

// The recognizer that recognizes `bcp47` ("en-US" or "en"): an exact tag first; for a
// tag without a region, a recognizer of that language, the default one first.
// Returns the index in `recognizers`, or -1.
int FindRecognizer(const std::vector<Recognizer>& recognizers, const std::wstring& bcp47);

enum class EventKind : int {
  kNone = 0,         // NextEvent timed out
  kSoundStart = 1,   // SPEI_SOUND_START: the engine hears sound (speech) in the stream
  kSoundEnd = 2,     // SPEI_SOUND_END
  kHypothesis = 3,   // SPEI_HYPOTHESIS: texts[0] is the partial text
  kRecognition = 4,  // SPEI_RECOGNITION: texts (best first) and confidences
  kNoMatch = 5,      // SPEI_FALSE_RECOGNITION: sound that matched nothing
  kStreamEnd = 6,    // SPEI_END_SR_STREAM: every byte handed over was processed
  kFailed = 7,       // the engine failed; hr says how
};

struct Event {
  EventKind kind = EventKind::kNone;
  std::vector<std::wstring> texts;
  // Per text: SPPHRASERULE.SREngineConfidence when it is within [0, 1], else -1
  // (Android's "unavailable" confidence).
  std::vector<float> confidences;
  HRESULT hr = S_OK;
};

class Session {
 public:
  virtual ~Session() = default;
  // Appends little-endian 16-bit mono PCM at the session's sample rate. Returns false
  // after EndAudio, Cancel or a failure.
  virtual bool Write(const int16_t* samples, size_t count) = 0;
  // No more audio: the engine recognizes what it has, then reports kStreamEnd.
  virtual void EndAudio() = 0;
  // Stops recognition now; later events are dropped and NextEvent reports kNone.
  // Callable from any thread, also while another thread waits in NextEvent.
  virtual void Cancel() = 0;
  // The next event, waiting up to timeout_ms (0: no wait).
  virtual Event NextEvent(DWORD timeout_ms) = 0;
};

struct SessionOptions {
  std::wstring language;  // BCP-47; see FindRecognizer
  int sample_rate = 16000;
  bool hypotheses = false;  // SPEI_HYPOTHESIS events
  int max_alternates = 1;   // texts per recognition, 1 or more
};

// Starts dictation (ISpRecoGrammar::LoadDictation) with the recognizer of
// options.language, reading the PCM the caller writes. Errors:
//   kSpErrNotFound  no installed recognizer recognizes the language
//   other           SAPI's HRESULT; *recognizer_failed is true when the matching
//                   token exists but its engine could not be created or loaded
HRESULT StartSession(const SessionOptions& options, std::unique_ptr<Session>* out,
                     bool* recognizer_failed);

}  // namespace gesher::devices::speech_recognition

#endif  // GESHER_COMPAT_DEVICES_SPEECH_RECOGNITION_WIN_H_
