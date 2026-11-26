// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Manual probe for Gesher's speech recognition natives (tests/speech/HUMAN_TEST.md):
// listens for 5 seconds on Windows' default microphone (waveIn, 16 kHz 16-bit mono) and
// hands the PCM to compat/devices/speech_recognition_win.cc, as the Android engine hands
// over AudioRecord's PCM. Prints every hypothesis and the final result.
//
//   recognize_probe.exe [language]     (default: the default SAPI recognizer's language)
//
// The probe captures with waveIn itself only because it runs without Android; in Gesher
// the audio comes from AudioRecord through Gesher's microphone path.

#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "speech_recognition_win.h"

namespace sr = gesher::devices::speech_recognition;

namespace {

std::string Narrow(const std::wstring& s) {
  if (s.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                              nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n, nullptr,
                      nullptr);
  return out;
}

const char* KindName(sr::EventKind k) {
  switch (k) {
    case sr::EventKind::kSoundStart: return "sound start";
    case sr::EventKind::kSoundEnd: return "sound end";
    case sr::EventKind::kHypothesis: return "hypothesis";
    case sr::EventKind::kRecognition: return "RESULT";
    case sr::EventKind::kNoMatch: return "no match";
    case sr::EventKind::kStreamEnd: return "stream end";
    case sr::EventKind::kFailed: return "FAILED";
    default: return "none";
  }
}

void Print(const sr::Event& e) {
  std::printf("[%s]", KindName(e.kind));
  for (size_t i = 0; i < e.texts.size(); ++i) {
    std::printf(" \"%s\" (confidence %.2f)", Narrow(e.texts[i]).c_str(),
                i < e.confidences.size() ? e.confidences[i] : -1.0f);
  }
  if (e.kind == sr::EventKind::kFailed) std::printf(" HRESULT 0x%08lX", static_cast<unsigned long>(e.hr));
  std::printf("\n");
  std::fflush(stdout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  std::vector<sr::Recognizer> recognizers;
  HRESULT hr = sr::EnumerateRecognizers(&recognizers);
  std::printf("recognizers: HRESULT 0x%08lX, %zu\n", static_cast<unsigned long>(hr), recognizers.size());
  std::wstring language;
  for (const sr::Recognizer& r : recognizers) {
    std::string langs;
    for (const std::wstring& l : r.languages) langs += (langs.empty() ? "" : ",") + Narrow(l);
    std::printf("  %s [%s]%s\n", Narrow(r.name).c_str(), langs.c_str(), r.is_default ? " (default)" : "");
    if (r.is_default && language.empty()) language = r.languages[0];
  }
  if (argc > 1) language = argv[1];
  if (language.empty() && !recognizers.empty()) language = recognizers[0].languages[0];
  if (language.empty()) {
    std::printf("no SAPI recognizer: Android apps get no recognition service\n");
    return 2;
  }

  sr::SessionOptions options;
  options.language = language;
  options.sample_rate = 16000;
  options.hypotheses = true;
  options.max_alternates = 3;
  std::unique_ptr<sr::Session> session;
  bool recognizer_failed = false;
  hr = sr::StartSession(options, &session, &recognizer_failed);
  std::printf("session %s: HRESULT 0x%08lX%s\n", Narrow(language).c_str(),
              static_cast<unsigned long>(hr), recognizer_failed ? " (recognizer failed)" : "");
  if (FAILED(hr)) return 1;

  WAVEFORMATEX format = {};
  format.wFormatTag = WAVE_FORMAT_PCM;
  format.nChannels = 1;
  format.nSamplesPerSec = 16000;
  format.wBitsPerSample = 16;
  format.nBlockAlign = 2;
  format.nAvgBytesPerSec = 32000;
  HWAVEIN in = nullptr;
  HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  MMRESULT mm = waveInOpen(&in, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(done), 0,
                           CALLBACK_EVENT);
  if (mm != MMSYSERR_NOERROR) {
    std::printf("waveInOpen failed: %u (microphone missing or blocked in Settings, Privacy)\n", mm);
    return 1;
  }
  constexpr int kBuffers = 4;
  constexpr DWORD kBytes = 3200;  // 100 ms
  std::vector<std::vector<char>> data(kBuffers, std::vector<char>(kBytes));
  WAVEHDR headers[kBuffers] = {};
  for (int i = 0; i < kBuffers; ++i) {
    headers[i].lpData = data[i].data();
    headers[i].dwBufferLength = kBytes;
    waveInPrepareHeader(in, &headers[i], sizeof(WAVEHDR));
    waveInAddBuffer(in, &headers[i], sizeof(WAVEHDR));
  }
  std::printf("listening for 5 seconds; speak now\n");
  std::fflush(stdout);
  waveInStart(in);
  ULONGLONG end = GetTickCount64() + 5000;
  while (GetTickCount64() < end) {
    WaitForSingleObject(done, 100);
    for (int i = 0; i < kBuffers; ++i) {
      if (!(headers[i].dwFlags & WHDR_DONE)) continue;
      session->Write(reinterpret_cast<const int16_t*>(headers[i].lpData),
                     headers[i].dwBytesRecorded / 2);
      headers[i].dwFlags &= ~WHDR_DONE;
      waveInAddBuffer(in, &headers[i], sizeof(WAVEHDR));
    }
    for (sr::Event e = session->NextEvent(0); e.kind != sr::EventKind::kNone; e = session->NextEvent(0)) {
      Print(e);
    }
  }
  waveInStop(in);
  waveInReset(in);
  for (int i = 0; i < kBuffers; ++i) waveInUnprepareHeader(in, &headers[i], sizeof(WAVEHDR));
  waveInClose(in);
  CloseHandle(done);
  session->EndAudio();
  std::printf("stopped listening; recognizing the rest\n");
  ULONGLONG deadline = GetTickCount64() + 20000;
  while (GetTickCount64() < deadline) {
    sr::Event e = session->NextEvent(500);
    if (e.kind == sr::EventKind::kNone) continue;
    Print(e);
    if (e.kind == sr::EventKind::kStreamEnd || e.kind == sr::EventKind::kFailed) break;
  }
  return 0;
}
