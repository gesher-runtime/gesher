// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Windows host test for Gesher's speech recognition natives (docs/speech-recognition.md):
// the production compat/devices/speech_recognition_win.cc against Windows' real SAPI 5,
// without a microphone and without UI. It enumerates the installed recognizers, checks
// that a language no recognizer has fails as the engine maps it (SPERR_NOT_FOUND, which
// the Android engine reports as ERROR_LANGUAGE_NOT_SUPPORTED), and, when the machine has
// a recognizer, recognizes two seconds of digital silence handed over as PCM: the stream
// must end (no recognition from silence) and a canceled session must stop at once.
//
//   cl /nologo /std:c++17 /EHsc /W4 /WX /Icompat\devices
//      tests\speech\speech_recognition_windows_test.cc compat\devices\speech_recognition_win.cc
//      /Fe:speech-recognition-windows.exe ole32.lib uuid.lib

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "speech_recognition_win.h"

namespace sr = gesher::devices::speech_recognition;

namespace {

int g_failures = 0;

// A startup or teardown deadlock must fail this process, never leave a detached
// worker or disguise a session that has not started as a successful test.
class Deadline {
 public:
  Deadline(const char* phase, DWORD timeout_ms)
      : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    if (done_ == nullptr) {
      std::fprintf(stderr, "FAIL could not create the test deadline event\n");
      std::exit(1);
    }
    watcher_ = std::thread([this, phase, timeout_ms] {
      if (WaitForSingleObject(done_, timeout_ms) != WAIT_OBJECT_0) {
        std::fprintf(stderr, "FAIL %s did not complete within %lu ms\n", phase, timeout_ms);
        std::fflush(stderr);
        std::_Exit(1);
      }
    });
  }
  ~Deadline() {
    SetEvent(done_);
    watcher_.join();
    CloseHandle(done_);
  }

 private:
  HANDLE done_;
  std::thread watcher_;
};

void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failures;
}

std::string Narrow(const std::wstring& s) {
  if (s.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                              nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n, nullptr,
                      nullptr);
  return out;
}

// Feeds `ms` of silence and returns the events until the stream ends (or 20 s pass).
std::vector<sr::Event> RecognizeSilence(sr::Session* session, int rate, int ms) {
  std::vector<int16_t> chunk(static_cast<size_t>(rate / 10), 0);  // 100 ms
  for (int t = 0; t < ms; t += 100) session->Write(chunk.data(), chunk.size());
  session->EndAudio();
  std::vector<sr::Event> events;
  ULONGLONG deadline = GetTickCount64() + 20000;
  while (GetTickCount64() < deadline) {
    sr::Event e = session->NextEvent(500);
    if (e.kind == sr::EventKind::kNone) continue;
    events.push_back(e);
    if (e.kind == sr::EventKind::kStreamEnd || e.kind == sr::EventKind::kFailed) break;
  }
  return events;
}

}  // namespace

int main() {
  // Preserve the last completed contract when a watchdog reports a deadlock.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  // The pure language matching, as the Android engine relies on it.
  std::vector<sr::Recognizer> fixture(2);
  fixture[0].token_id = L"A";
  fixture[0].languages = {L"en-GB"};
  fixture[1].token_id = L"B";
  fixture[1].languages = {L"en-US", L"en"};
  fixture[1].is_default = true;
  Check(sr::FindRecognizer(fixture, L"en-GB") == 0, "exact tag selects its recognizer");
  Check(sr::FindRecognizer(fixture, L"EN-us") == 1, "tags compare without case");
  Check(sr::FindRecognizer(fixture, L"en") == 1, "a bare language prefers the default recognizer");
  Check(sr::FindRecognizer(fixture, L"en-AU") == -1, "another region is not substituted");
  Check(sr::FindRecognizer(fixture, L"he-IL") == -1, "a language without recognizer is not found");
  Check(sr::FindRecognizer(fixture, L"") == -1, "no language is not found");

  std::vector<sr::Recognizer> recognizers;
  HRESULT hr = sr::EnumerateRecognizers(&recognizers);
  std::printf("EnumerateRecognizers: HRESULT 0x%08lX, %zu recognizer(s)\n",
              static_cast<unsigned long>(hr), recognizers.size());
  Check(SUCCEEDED(hr), "SAPI enumerates its recognizers");
  for (const sr::Recognizer& r : recognizers) {
    std::string langs;
    for (const std::wstring& l : r.languages) langs += (langs.empty() ? "" : ",") + Narrow(l);
    std::printf("  %s [%s]%s\n", Narrow(r.name).c_str(), langs.c_str(),
                r.is_default ? " (default)" : "");
    Check(!r.token_id.empty() && !r.languages.empty(), "a recognizer has a token and languages");
  }

  std::unique_ptr<sr::Session> session;
  bool recognizer_failed = true;
  sr::SessionOptions unsupported;
  unsupported.language = L"x-gesher-none";
  hr = sr::StartSession(unsupported, &session, &recognizer_failed);
  Check(hr == sr::kSpErrNotFound && session == nullptr && !recognizer_failed,
        "a language without recognizer fails with SPERR_NOT_FOUND");

  sr::SessionOptions invalid;
  invalid.language = L"en-US";
  invalid.sample_rate = 1000;
  Check(sr::StartSession(invalid, &session, &recognizer_failed) == E_INVALIDARG,
        "an unusable sample rate is refused");

  if (recognizers.empty()) {
    std::printf("SKIP recognition: this machine has no SAPI recognizer\n");
  } else {
    sr::SessionOptions options;
    options.language = recognizers[0].languages[0];
    options.sample_rate = 16000;
    options.hypotheses = true;
    options.max_alternates = 3;
    {
      Deadline deadline("starting dictation before caller PCM", 30000);
      hr = sr::StartSession(options, &session, &recognizer_failed);
    }
    std::printf("StartSession(%s): HRESULT 0x%08lX%s\n", Narrow(options.language).c_str(),
                static_cast<unsigned long>(hr), recognizer_failed ? " (recognizer failed)" : "");
    Check(SUCCEEDED(hr) && session != nullptr, "dictation starts before the caller provides PCM");
    if (session != nullptr) {
      std::vector<sr::Event> events = RecognizeSilence(session.get(), 16000, 2000);
      bool ended = !events.empty() && events.back().kind == sr::EventKind::kStreamEnd;
      bool recognized = false;
      for (const sr::Event& e : events) recognized |= e.kind == sr::EventKind::kRecognition;
      Check(ended, "the stream ends after EndAudio");
      Check(!recognized, "silence is not recognized as speech");
      Check(!session->Write(std::vector<int16_t>(160).data(), 160),
            "no audio is taken after the end");
      {
        Deadline deadline("releasing the ended stream", 5000);
        session.reset();
      }

      {
        Deadline deadline("starting the empty session to cancel", 30000);
        hr = sr::StartSession(options, &session, &recognizer_failed);
      }
      Check(SUCCEEDED(hr) && session != nullptr,
            "a second session starts without PCM before immediate cancellation");
      if (session != nullptr) {
        Deadline deadline("canceling and joining a session with no PCM", 5000);
        ULONGLONG before = GetTickCount64();
        session->Cancel();
        Check(session->NextEvent(1000).kind == sr::EventKind::kNone,
              "a canceled session reports nothing more");
        Check(!session->Write(std::vector<int16_t>(160).data(), 160),
              "a canceled session takes no more audio");
        session.reset();
        Check(GetTickCount64() - before < 5000, "cancel releases the session promptly");
      }
    }
  }

  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
