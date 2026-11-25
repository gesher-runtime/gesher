// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Windows host contracts for compat/devices/speech_win.cc (docs/tts.md), against the
// real Windows.Media.SpeechSynthesis without any UI or audio output:
//  - the voices enumerate (none is an answer, not a failure) with an Id and a BCP-47
//    language each, and at most one default;
//  - with a voice: a short text yields a RIFF WAV of PCM that is not silence; a faster
//    SpeakingRate yields shorter audio; word boundaries, when Windows gives them, name
//    the words of the input (EndPositionInInput is the word's last character);
//  - an unknown voice Id never starts; a canceled synthesis ends canceled or done,
//    never failed.
// tests/tts/tts_probe.cc is the by-hand probe.

#include <windows.h>
#include <stdio.h>

#include <string>
#include <vector>

#include "wav_info.h"
#include "speech_win.h"

using namespace gesher::devices;

static int failures, checks;

static void Check(bool ok, const char* what) {
  checks++;
  if (!ok) failures++;
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

struct Result {
  int status = -1;
  HRESULT hr = E_FAIL;
  speech::StartFailure failure = speech::StartFailure::kNone;
  std::vector<uint8_t> audio;
  std::vector<speech::Word> words;
};

static Result Synthesize(const std::wstring& text, const std::wstring& voice, double rate,
                         double pitch, bool words) {
  Result r;
  speech::Synthesis* s = speech::Start(text.data(), text.size(), voice.c_str(), rate, pitch, words,
                                       &r.failure, &r.hr);
  if (s == nullptr) return r;
  r.status = speech::Await(s, 60000, &r.hr);
  if (r.status == speech::kDone) {
    r.audio = speech::Audio(s);
    r.words = speech::Words(s);
  }
  speech::Release(s);
  return r;
}

int main() {
  std::vector<speech::Voice> voices;
  HRESULT hr = E_FAIL;
  bool ok = speech::Voices(&voices, &hr);
  printf("voices=%zu hr=0x%08lx\n", voices.size(), (unsigned long)hr);
  Check(ok && SUCCEEDED(hr), "SpeechSynthesizer.AllVoices enumerates without UI");
  int defaults = 0;
  bool described = true;
  for (const speech::Voice& v : voices) {
    printf("  %ls %ls gender=%d%s\n", v.language.c_str(), v.displayName.c_str(), v.gender,
           v.isDefault ? " default" : "");
    described = described && !v.id.empty() && v.language.find(L'-') != std::wstring::npos &&
                (v.gender == 0 || v.gender == 1);
    defaults += v.isDefault ? 1 : 0;
  }
  Check(described, "every voice has an Id, a language-region tag and a VoiceGender");
  Check(defaults <= 1, "at most one voice is the default");
  std::vector<speech::Voice> again;
  Check(speech::Voices(&again, &hr) && again.size() == voices.size(),
        "the voices are stable across queries");

  speech::StartFailure failure = speech::StartFailure::kNone;
  const std::wstring text = L"hello world";
  speech::Synthesis* none = speech::Start(text.data(), text.size(), L"no such voice", 1.0, 1.0,
                                          false, &failure, &hr);
  Check(none == nullptr && failure == speech::StartFailure::kNoSuchVoice,
        "an unknown voice Id never starts a synthesis");
  if (none != nullptr) speech::Release(none);

  const speech::Voice* en = nullptr;
  for (const speech::Voice& v : voices) {
    if (en == nullptr && v.language.compare(0, 3, L"en-") == 0) en = &v;
  }
  const speech::Voice* voice = en != nullptr ? en : voices.empty() ? nullptr : &voices[0];
  if (voice == nullptr) {
    printf("SKIP synthesis: this Windows has no voices (Gesher installs no engine here)\n");
  } else {
    printf("voice: %ls (%ls)\n", voice->displayName.c_str(), voice->language.c_str());
    Result normal = Synthesize(text, voice->id, 1.0, 1.0, true);
    WavInfo w = ParseWav(normal.audio);
    printf("status=%d hr=0x%08lx format=%d channels=%d rate=%d bits=%d data=%zu words=%zu\n",
           normal.status, (unsigned long)normal.hr, w.format, w.channels, w.sampleRate,
           w.bitsPerSample, w.dataBytes, normal.words.size());
    Check(normal.status == speech::kDone, "a short text synthesizes");
    Check(w.ok && w.format == 1 && w.bitsPerSample == 16 && (w.channels == 1 || w.channels == 2) &&
              w.sampleRate >= 8000,
          "the stream is RIFF WAV of 16-bit PCM, mono or stereo");
    Check(w.dataBytes > 0 && Peak16(normal.audio, w) > 0, "the PCM is speech, not silence");

    if (!normal.words.empty() && en != nullptr) {
      bool named = true;
      for (const speech::Word& word : normal.words) {
        bool inside = word.first >= 0 && word.last >= word.first &&
                      static_cast<size_t>(word.last) < text.size();
        std::wstring spoken = inside ? text.substr(word.first, word.last - word.first + 1) : L"";
        printf("  word %.3fs [%d..%d] '%ls'\n", word.startTicks / 1e7, word.first, word.last,
               spoken.c_str());
        named = named && (spoken == L"hello" || spoken == L"world");
      }
      Check(named, "word boundaries cover exactly the input's words (last is inclusive)");
    } else {
      printf("SKIP word boundaries: none reported for this voice\n");
    }

    Result fast = Synthesize(text, voice->id, 2.0, 1.0, false);
    WavInfo f = ParseWav(fast.audio);
    printf("rate 2.0: status=%d %.3fs vs %.3fs\n", fast.status, Seconds(f), Seconds(w));
    Check(fast.status == speech::kDone && f.ok && Seconds(f) < Seconds(w),
          "SpeakingRate 2.0 is shorter than 1.0");

    std::wstring longText;
    for (int i = 0; i < 100; i++) longText += L"one two three four five six seven eight. ";
    speech::Synthesis* s = speech::Start(longText.data(), longText.size(), voice->id.c_str(), 1.0,
                                         1.0, false, &failure, &hr);
    Check(s != nullptr, "a long synthesis starts");
    if (s != nullptr) {
      speech::Cancel(s);
      speech::Cancel(s);  // idempotent
      int status = speech::Await(s, 60000, &hr);
      printf("canceled: status=%d hr=0x%08lx\n", status, (unsigned long)hr);
      Check(status == speech::kCanceled || status == speech::kDone,
            "a canceled synthesis ends canceled (or done, if it won the race)");
      speech::Release(s);
    }
  }

  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
