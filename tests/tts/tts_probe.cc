// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// A by-hand probe of compat/devices/speech_win.cc on a real Windows (docs/tts.md):
// lists the installed voices, synthesizes a word with the Hebrew voice when Windows
// has one (else an en-US voice, else the first), writes the stream to a WAV file
// and prints its format, duration, peak and word boundaries; then cancels a long
// synthesis. Nothing is played. Build (Developer Command Prompt):
//   cl /nologo /std:c++17 /EHsc /W4 /D_WIN32_WINNT=0x0A00 /Icompat\devices
//      tests\tts\tts_probe.cc compat\devices\speech_win.cc compat\devices\winrt.cc
//      /Fe:tts-probe.exe ole32.lib uuid.lib
// Run: tts-probe.exe [--lang <bcp47>] [--text <text>] [--rate <0.5..6>]
//      [--pitch <0..2>] [--out <file.wav>]

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include "wav_info.h"
#include "speech_win.h"

using namespace gesher::devices;

static void PrintW(const wchar_t* label, const std::wstring& s) {
  // UTF-8 to the console, so Hebrew names print.
  int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0,
                              nullptr, nullptr);
  std::string utf8(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &utf8[0], n, nullptr,
                      nullptr);
  printf("%ls%s", label, utf8.c_str());
}

static bool StartsWithLanguage(const std::wstring& tag, const std::wstring& language) {
  return _wcsnicmp(tag.c_str(), language.c_str(), language.size()) == 0 &&
         (tag.size() == language.size() || tag[language.size()] == L'-');
}

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  std::wstring lang, text, out = L"tts-probe.wav";
  double rate = 1.0, pitch = 1.0;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::wstring key = argv[i];
    if (key == L"--lang") lang = argv[i + 1];
    else if (key == L"--text") text = argv[i + 1];
    else if (key == L"--rate") rate = _wtof(argv[i + 1]);
    else if (key == L"--pitch") pitch = _wtof(argv[i + 1]);
    else if (key == L"--out") out = argv[i + 1];
  }

  std::vector<speech::Voice> voices;
  HRESULT hr = S_OK;
  if (!speech::Voices(&voices, &hr)) {
    printf("FAIL: Windows speech synthesis unavailable: HRESULT 0x%08lx\n", (unsigned long)hr);
    return 1;
  }
  printf("voices: %zu\n", voices.size());
  for (const speech::Voice& v : voices) {
    PrintW(L"  ", v.language);
    PrintW(L"  ", v.displayName);
    printf("  %s%s\n", v.gender == 1 ? "female" : "male", v.isDefault ? "  (default)" : "");
    PrintW(L"      id=", v.id);
    printf("\n");
  }
  if (voices.empty()) {
    printf("no voices: Gesher installs no text-to-speech engine on this PC\n");
    return 0;
  }

  const speech::Voice* chosen = nullptr;
  const wchar_t* wanted[] = {lang.empty() ? L"he-IL" : lang.c_str(), L"en-US"};
  for (const wchar_t* w : wanted) {
    for (const speech::Voice& v : voices) {
      if (chosen == nullptr && StartsWithLanguage(v.language, w)) chosen = &v;
    }
  }
  if (chosen == nullptr) chosen = &voices[0];
  if (text.empty()) {
    text = StartsWithLanguage(chosen->language, L"he") ? L"שלום" : L"shalom";
  }
  PrintW(L"synthesizing with ", chosen->displayName);
  PrintW(L" (", chosen->language);
  PrintW(L"): ", text);
  printf("  rate=%.2f pitch=%.2f\n", rate, pitch);

  speech::StartFailure failure = speech::StartFailure::kNone;
  ULONGLONG t0 = GetTickCount64();
  speech::Synthesis* s = speech::Start(text.data(), text.size(), chosen->id.c_str(), rate, pitch,
                                       true, &failure, &hr);
  if (s == nullptr) {
    printf("FAIL: synthesis did not start: failure=%d HRESULT 0x%08lx\n", (int)failure,
           (unsigned long)hr);
    return 1;
  }
  int status = speech::Await(s, 60000, &hr);
  ULONGLONG t1 = GetTickCount64();
  if (status != speech::kDone) {
    printf("FAIL: synthesis ended with %d, HRESULT 0x%08lx\n", status, (unsigned long)hr);
    speech::Release(s);
    return 1;
  }
  const std::vector<uint8_t>& audio = speech::Audio(s);
  WavInfo w = ParseWav(audio);
  printf("stream: %zu bytes in %llu ms\n", audio.size(), (unsigned long long)(t1 - t0));
  printf("wav: ok=%d format=%d channels=%d sampleRate=%d bits=%d data=%zu bytes\n", w.ok,
         w.format, w.channels, w.sampleRate, w.bitsPerSample, w.dataBytes);
  printf("duration: %.3f s\n", Seconds(w));
  if (w.ok && w.bitsPerSample == 16) printf("peak: %d of 32767\n", Peak16(audio, w));
  for (const speech::Word& word : speech::Words(s)) {
    bool inside = word.first >= 0 && word.last >= word.first &&
                  static_cast<size_t>(word.last) < text.size();
    printf("word at %.3f s: [%d..%d]", word.startTicks / 1e7, word.first, word.last);
    if (inside) PrintW(L" ", text.substr(word.first, word.last - word.first + 1));
    printf("%s\n", inside ? "" : " (outside the text)");
  }
  FILE* f = nullptr;
  if (_wfopen_s(&f, out.c_str(), L"wb") == 0 && f != nullptr) {
    fwrite(audio.data(), 1, audio.size(), f);
    fclose(f);
    PrintW(L"wrote ", out);
    printf("\n");
  }
  speech::Release(s);

  // Cancel: a long text, canceled 50 ms in.
  std::wstring longText;
  for (int i = 0; i < 200; i++) longText += L"one two three four five six seven eight nine ten. ";
  s = speech::Start(longText.data(), longText.size(), chosen->id.c_str(), 1.0, 1.0, false,
                    &failure, &hr);
  if (s != nullptr) {
    Sleep(50);
    speech::Cancel(s);
    status = speech::Await(s, 60000, &hr);
    printf("cancel after 50 ms: %s\n", status == speech::kCanceled ? "canceled"
                                       : status == speech::kDone   ? "completed first"
                                                                   : "FAILED");
    speech::Release(s);
  }
  printf(w.ok && w.dataBytes > 0 ? "PROBE OK\n" : "FAIL: no PCM\n");
  return w.ok && w.dataBytes > 0 ? 0 : 1;
}
