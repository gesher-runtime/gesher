// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Gesher (devices track): Windows speech synthesis
// (Windows.Media.SpeechSynthesis.SpeechSynthesizer) through its ABI, for the
// Gesher text-to-speech engine (app/gesher-tts, docs/tts.md). No UI and no
// audio output: Windows renders the utterance into a WAV stream, which the
// engine hands to Android's SynthesisCallback.
#pragma once

#include <windows.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace gesher::devices::speech {

// One installed voice (SpeechSynthesizer.AllVoices, VoiceInformation).
struct Voice {
  std::wstring id;           // VoiceInformation.Id
  std::wstring displayName;  // VoiceInformation.DisplayName
  std::wstring language;     // VoiceInformation.Language: a normalized BCP-47 tag
  int gender = 0;            // VoiceGender: Male 0, Female 1
  bool isDefault = false;    // SpeechSynthesizer.DefaultVoice has the same Id
};

// The installed voices. False, with *hr, when Windows has no speech synthesis
// (WinRT or the class missing); true with an empty list when it has no voices.
bool Voices(std::vector<Voice>* voices, HRESULT* hr);

// A word of the input (SpeechCue of the "SpeechWord" track): when it starts in
// the audio, and its first and last UTF-16 units in the input text.
struct Word {
  int64_t startTicks = 0;  // 100 ns from the start of the stream
  int32_t first = 0;
  int32_t last = 0;  // inclusive, as SpeechCue.EndPositionInInput
};

// Why Start returned no synthesis.
enum class StartFailure { kNone, kWindows, kNoSuchVoice, kProsodyUnsupported };

struct Synthesis;

// Starts synthesizing `text` (UTF-16, plain text: no SSML) with the voice whose
// VoiceInformation.Id is `voiceId`, at `rate` (SpeakingRate, 0.5..6.0) and
// `pitch` (AudioPitch, 0.0..2.0); out-of-range values are the caller's error
// (kWindows). With `words`, word boundaries are requested
// (IncludeWordBoundaryMetadata). Null on failure, with *failure and *hr.
Synthesis* Start(const wchar_t* text, size_t length, const wchar_t* voiceId, double rate,
                 double pitch, bool words, StartFailure* failure, HRESULT* hr);

// What Await returns.
constexpr int kDone = 0;
constexpr int kCanceled = 1;
constexpr int kFailed = 2;

// Blocks until the synthesis ends (or `timeoutMs` passes: kFailed with
// ERROR_TIMEOUT), then reads the stream. kDone: Audio and Words are set.
int Await(Synthesis* s, int64_t timeoutMs, HRESULT* hr);

// The stream's bytes (a RIFF WAV container as Windows wrote it) and the words,
// after Await returned kDone.
const std::vector<uint8_t>& Audio(const Synthesis* s);
const std::vector<Word>& Words(const Synthesis* s);

// Thread-safe and idempotent until Release: asks Windows to cancel.
void Cancel(Synthesis* s);

// Frees the synthesis; Await must have returned (or never been called).
void Release(Synthesis* s);

}  // namespace gesher::devices::speech
