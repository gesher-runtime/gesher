// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Gesher (devices track, docs/speech-recognition.md): SAPI 5's in-process recognizer
// over PCM that Android's AudioRecord captured. See speech_recognition_win.h.
//
// Each session owns one thread that creates, drives and releases every SAPI object
// of the session (multithreaded apartment). The thread waits on the recognition
// context's Win32 notification event and on a stop event, and queues SAPI's events
// for NextEvent. The PCM pipe is the session's input stream: Write appends, the
// engine's reads block until enough audio is there, and after EndAudio (or Cancel)
// a short or empty read ends the stream, which SAPI reports as SPEI_END_SR_STREAM.

#include "speech_recognition_win.h"

#include <objbase.h>
#include <sapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace gesher::devices::speech_recognition {
namespace {

// Diagnostic-only tracing; no production behavior change.
void Trace(const char* phase) {
  std::fprintf(stderr, "TRACE tid=%lu %s\n", GetCurrentThreadId(), phase);
  std::fflush(stderr);
}

// sapi.h's class IDs (Windows SDK sapi.idl, coclass uuid attributes). Declared here so
// that only ole32 and uuid are linked, as the other device natives.
constexpr CLSID kClsidSpInprocRecognizer = {
    0x41b89b6b, 0x9399, 0x11d2, {0x96, 0x23, 0x00, 0xc0, 0x4f, 0x8e, 0xe6, 0x28}};
constexpr CLSID kClsidSpStream = {
    0x715d9c59, 0x4442, 0x11d2, {0x96, 0x05, 0x00, 0xc0, 0x4f, 0x8e, 0xe6, 0x28}};
constexpr CLSID kClsidSpObjectTokenCategory = {
    0xa910187f, 0x0c7a, 0x45ac, {0x92, 0xcc, 0x59, 0xed, 0xaf, 0xb7, 0x7b, 0x53}};
constexpr CLSID kClsidSpObjectToken = {
    0xef411752, 0x3736, 0x4cb4, {0x9c, 0x8c, 0x8e, 0xf4, 0xcc, 0xb5, 0x8e, 0xfe}};
// SPDFID_WaveFormatEx: the stream's format is the WAVEFORMATEX passed with it. sapi.h only
// declares it (sapi.lib defines it); the value is sapi.h's SpeechAudioFormatGUIDWave,
// L"{C31ADBAE-527F-4ff5-A230-F62BB61FF70C}".
constexpr GUID kSpdfidWaveFormatEx = {
    0xc31adbae, 0x527f, 0x4ff5, {0xa2, 0x30, 0xf6, 0x2b, 0xb6, 0x1f, 0xf7, 0x0c}};

constexpr wchar_t kRecognizersCategory[] =
    L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Recognizers";  // SPCAT_RECOGNIZERS

// sapi.h's SPFEI(SPEI_ord): ((1ui64 << SPEI_ord) | SPFEI_FLAGCHECK), SPFEI_FLAGCHECK being
// the SPEI_RESERVED1 and SPEI_RESERVED2 bits.
constexpr ULONGLONG Interest(SPEVENTENUM e) {
  return (1ULL << e) | (1ULL << SPEI_RESERVED1) | (1ULL << SPEI_RESERVED2);
}

template <typename T>
class Ref {
 public:
  Ref() = default;
  ~Ref() { Reset(); }
  Ref(const Ref&) = delete;
  Ref& operator=(const Ref&) = delete;
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  T** put() {
    Reset();
    return &p_;
  }
  void Reset() {
    if (p_ != nullptr) p_->Release();
    p_ = nullptr;
  }

 private:
  T* p_ = nullptr;
};

class CoTaskString {
 public:
  ~CoTaskString() { CoTaskMemFree(s_); }
  WCHAR** put() { return &s_; }
  const WCHAR* get() const { return s_; }

 private:
  WCHAR* s_ = nullptr;
};

HRESULT Log(const char* what, HRESULT hr) {
  fprintf(stderr, "speech recognition: %s failed: HRESULT 0x%08lX\n", what,
          static_cast<unsigned long>(hr));
  return hr;
}

// "409;9" -> {"en-US", "en"}: the token's hexadecimal LCIDs as locale names.
std::vector<std::wstring> LanguagesOf(const std::wstring& attribute) {
  std::vector<std::wstring> tags;
  size_t start = 0;
  while (start <= attribute.size()) {
    size_t end = attribute.find(L';', start);
    if (end == std::wstring::npos) end = attribute.size();
    std::wstring hex = attribute.substr(start, end - start);
    start = end + 1;
    if (hex.empty()) continue;
    wchar_t* stop = nullptr;
    unsigned long lcid = wcstoul(hex.c_str(), &stop, 16);
    if (stop == nullptr || *stop != L'\0' || lcid == 0) continue;
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    int length = LCIDToLocaleName(static_cast<LCID>(lcid), name, LOCALE_NAME_MAX_LENGTH, 0);
    if (length <= 1) continue;
    std::wstring tag(name, static_cast<size_t>(length - 1));
    if (std::find(tags.begin(), tags.end(), tag) == tags.end()) tags.push_back(tag);
  }
  return tags;
}

bool SameTag(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                              static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring PrimaryOf(const std::wstring& tag) { return tag.substr(0, tag.find(L'-')); }

// COM on the calling thread, multithreaded unless the thread already chose STA.
class ComScope {
 public:
  ComScope() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComScope() {
    if (SUCCEEDED(hr_)) CoUninitialize();
  }
  HRESULT status() const { return hr_ == RPC_E_CHANGED_MODE ? S_OK : hr_; }

 private:
  HRESULT hr_;
};

// The session's input: an IStream the engine reads PCM from.
class PcmPipe final : public IStream {
 public:
  PcmPipe() = default;

  bool Append(const int16_t* samples, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ended_) return false;
    const BYTE* bytes = reinterpret_cast<const BYTE*>(samples);
    buffer_.insert(buffer_.end(), bytes, bytes + count * sizeof(int16_t));
    ready_.notify_all();
    return true;
  }

  void End() {
    std::lock_guard<std::mutex> lock(mutex_);
    Trace("PcmPipe::End");
    ended_ = true;
    ready_.notify_all();
  }

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (iid == IID_IUnknown || iid == IID_ISequentialStream || iid == IID_IStream) {
      *out = static_cast<IStream*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG refs = static_cast<ULONG>(InterlockedDecrement(&refs_));
    if (refs == 0) delete this;
    return refs;
  }

  // ISequentialStream. A read returns all `size` bytes once they are there; after End
  // it returns what is left, which is shorter: the end of the stream.
  STDMETHODIMP Read(void* data, ULONG size, ULONG* read) override {
    if (data == nullptr) return STG_E_INVALIDPOINTER;
    std::unique_lock<std::mutex> lock(mutex_);
    const bool trace_read = ++trace_reads_ <= 8;
    if (trace_read) {
      std::fprintf(stderr, "TRACE tid=%lu Read enter requested=%lu buffered=%zu ended=%d\n",
                   GetCurrentThreadId(), size, buffer_.size(), ended_ ? 1 : 0);
      std::fflush(stderr);
    }
    ready_.wait(lock, [&] { return ended_ || buffer_.size() >= size; });
    ULONG n = static_cast<ULONG>(std::min<size_t>(size, buffer_.size()));
    std::copy(buffer_.begin(), buffer_.begin() + n, static_cast<BYTE*>(data));
    buffer_.erase(buffer_.begin(), buffer_.begin() + n);
    position_ += n;
    if (read != nullptr) *read = n;
    if (trace_read) {
      std::fprintf(stderr, "TRACE tid=%lu Read return requested=%lu read=%lu buffered=%zu ended=%d hr=S_OK\n",
                   GetCurrentThreadId(), size, n, buffer_.size(), ended_ ? 1 : 0);
      std::fflush(stderr);
    }
    return S_OK;
  }
  STDMETHODIMP Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }

  // IStream: a forward-only stream; its position can be asked for, not changed.
  STDMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override {
    std::lock_guard<std::mutex> lock(mutex_);
    bool here = (origin == STREAM_SEEK_CUR && move.QuadPart == 0) ||
                (origin == STREAM_SEEK_SET && move.QuadPart >= 0 &&
                 static_cast<ULONGLONG>(move.QuadPart) == position_);
    if (!here) return STG_E_INVALIDFUNCTION;
    if (result != nullptr) result->QuadPart = position_;
    return S_OK;
  }
  STDMETHODIMP SetSize(ULARGE_INTEGER) override { return STG_E_INVALIDFUNCTION; }
  STDMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP Commit(DWORD) override { return S_OK; }
  STDMETHODIMP Revert() override { return STG_E_REVERTED; }
  STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }
  STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }
  STDMETHODIMP Stat(STATSTG* stat, DWORD) override {
    if (stat == nullptr) return STG_E_INVALIDPOINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    ZeroMemory(stat, sizeof(*stat));
    stat->type = STGTY_STREAM;
    // A live stream: its size is what was read plus what is waiting.
    stat->cbSize.QuadPart = position_ + buffer_.size();
    stat->grfMode = STGM_READ;
    return S_OK;
  }
  STDMETHODIMP Clone(IStream**) override { return E_NOTIMPL; }

 private:
  ~PcmPipe() = default;

  LONG refs_ = 1;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<BYTE> buffer_;
  ULONGLONG position_ = 0;
  bool ended_ = false;
  unsigned trace_reads_ = 0;
};

HRESULT TokenLanguages(ISpObjectToken* token, std::vector<std::wstring>* out) {
  Ref<ISpDataKey> attributes;
  HRESULT hr = token->OpenKey(L"Attributes", attributes.put());
  if (FAILED(hr)) return hr;
  CoTaskString value;
  hr = attributes->GetStringValue(L"Language", value.put());
  if (FAILED(hr)) return hr;
  *out = LanguagesOf(value.get() == nullptr ? std::wstring() : std::wstring(value.get()));
  return S_OK;
}

HRESULT Enumerate(std::vector<Recognizer>* out) {
  Ref<ISpObjectTokenCategory> category;
  HRESULT hr = CoCreateInstance(kClsidSpObjectTokenCategory, nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(ISpObjectTokenCategory),
                                reinterpret_cast<void**>(category.put()));
  if (FAILED(hr)) {
    Log("CoCreateInstance(SpObjectTokenCategory)", hr);
    return hr;
  }
  // fCreateIfNotExist FALSE: a PC without recognizers has no such key.
  hr = category->SetId(kRecognizersCategory, FALSE);
  if (FAILED(hr)) return hr == kSpErrNotFound ? S_OK : hr;
  CoTaskString default_id;
  if (FAILED(category->GetDefaultTokenId(default_id.put()))) *default_id.put() = nullptr;
  Ref<IEnumSpObjectTokens> tokens;
  hr = category->EnumTokens(nullptr, nullptr, tokens.put());
  if (FAILED(hr)) {
    Log("ISpObjectTokenCategory::EnumTokens", hr);
    return hr;
  }
  ULONG count = 0;
  hr = tokens->GetCount(&count);
  if (FAILED(hr)) return hr;
  for (ULONG i = 0; i < count; ++i) {
    Ref<ISpObjectToken> token;
    if (FAILED(tokens->Item(i, token.put()))) continue;
    CoTaskString id;
    if (FAILED(token->GetId(id.put())) || id.get() == nullptr) continue;
    Recognizer r;
    r.token_id = id.get();
    CoTaskString name;
    if (SUCCEEDED(token->GetStringValue(nullptr, name.put())) && name.get() != nullptr) {
      r.name = name.get();
    }
    if (FAILED(TokenLanguages(token.get(), &r.languages)) || r.languages.empty()) continue;
    r.is_default = default_id.get() != nullptr && SameTag(r.token_id, default_id.get());
    out->push_back(std::move(r));
  }
  return S_OK;
}

// The texts of a recognition: the result's own text, then its alternates'.
void ReadResult(ISpRecoResult* result, int max_alternates, Event* event) {
  max_alternates = std::clamp(max_alternates, 1, kMaxAlternates);
  auto add = [&](ISpPhrase* phrase) {
    CoTaskString text;
    if (FAILED(phrase->GetText(static_cast<ULONG>(SPPR_ALL_ELEMENTS),
                               static_cast<ULONG>(SPPR_ALL_ELEMENTS), TRUE, text.put(), nullptr)) ||
        text.get() == nullptr) {
      return;
    }
    std::wstring s(text.get());
    if (s.empty() || std::find(event->texts.begin(), event->texts.end(), s) != event->texts.end()) {
      return;
    }
    float confidence = -1.0f;
    SPPHRASE* info = nullptr;
    if (SUCCEEDED(phrase->GetPhrase(&info)) && info != nullptr) {
      float c = info->Rule.SREngineConfidence;
      if (c >= 0.0f && c <= 1.0f) confidence = c;
      CoTaskMemFree(info);
    }
    event->texts.push_back(std::move(s));
    event->confidences.push_back(confidence);
  };
  add(result);
  if (max_alternates <= 1 || event->texts.empty()) return;
  std::vector<ISpPhraseAlt*> alternates(static_cast<size_t>(max_alternates), nullptr);
  ULONG returned = 0;
  if (SUCCEEDED(result->GetAlternates(0, static_cast<ULONG>(SPPR_ALL_ELEMENTS),
                                      static_cast<ULONG>(max_alternates), alternates.data(),
                                      &returned))) {
    for (ULONG i = 0; i < returned && i < alternates.size(); ++i) {
      if (alternates[i] == nullptr) continue;
      if (event->texts.size() < static_cast<size_t>(max_alternates)) add(alternates[i]);
      alternates[i]->Release();
    }
  }
}

class SapiSession final : public Session {
 public:
  SapiSession(const SessionOptions& options, std::wstring token_id)
      : options_(options), token_id_(std::move(token_id)) {
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }

  ~SapiSession() override {
    Cancel();
    Trace("before destructor joins worker");
    if (thread_.joinable()) thread_.join();
    Trace("after destructor joins worker");
    if (pipe_ != nullptr) pipe_->Release();
    if (stop_ != nullptr) CloseHandle(stop_);
  }

  // Runs the setup on the session's thread; returns its HRESULT.
  HRESULT Start(bool* recognizer_failed) {
    if (stop_ == nullptr) return HRESULT_FROM_WIN32(GetLastError());
    pipe_ = new PcmPipe();
    std::unique_lock<std::mutex> lock(mutex_);
    thread_ = std::thread([this] { Run(); });
    Trace("before Start waits setup_done");
    started_.wait(lock, [&] { return setup_done_; });
    Trace("after Start waits setup_done");
    *recognizer_failed = recognizer_failed_;
    return setup_hr_;
  }

  bool Write(const int16_t* samples, size_t count) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (canceled_ || failed_) return false;
    }
    return pipe_->Append(samples, count);
  }

  void EndAudio() override { pipe_->End(); }

  void Cancel() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (canceled_) return;
      canceled_ = true;
      events_.clear();
      queued_.notify_all();
    }
    if (pipe_ != nullptr) pipe_->End();
    if (stop_ != nullptr) SetEvent(stop_);
  }

  Event NextEvent(DWORD timeout_ms) override {
    std::unique_lock<std::mutex> lock(mutex_);
    (void)queued_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [&] { return canceled_ || !events_.empty(); });
    if (canceled_ || events_.empty()) return Event();
    Event e = std::move(events_.front());
    events_.pop_front();
    return e;
  }

 private:
  void Queue(Event e) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (canceled_) return;
    if (e.kind == EventKind::kFailed) failed_ = true;
    events_.push_back(std::move(e));
    queued_.notify_all();
  }

  void SetupDone(HRESULT hr, bool recognizer_failed) {
    std::lock_guard<std::mutex> lock(mutex_);
    setup_hr_ = hr;
    recognizer_failed_ = recognizer_failed;
    setup_done_ = true;
    started_.notify_all();
  }

  HRESULT Setup(Ref<ISpRecognizer>* recognizer, Ref<ISpRecoContext>* context,
                Ref<ISpRecoGrammar>* grammar, bool* recognizer_failed) {
    HRESULT hr = CoCreateInstance(kClsidSpInprocRecognizer, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(ISpRecognizer),
                                  reinterpret_cast<void**>(recognizer->put()));
    if (FAILED(hr)) return Log("CoCreateInstance(SpInprocRecognizer)", hr);
    Ref<ISpObjectToken> token;
    hr = CoCreateInstance(kClsidSpObjectToken, nullptr, CLSCTX_INPROC_SERVER,
                          __uuidof(ISpObjectToken), reinterpret_cast<void**>(token.put()));
    if (FAILED(hr)) return Log("CoCreateInstance(SpObjectToken)", hr);
    Trace("before token SetId");
    hr = token->SetId(nullptr, token_id_.c_str(), FALSE);
    Trace("after token SetId");
    Trace("before SetRecognizer");
    if (SUCCEEDED(hr)) hr = (*recognizer)->SetRecognizer(token.get());
    Trace("after SetRecognizer");
    if (FAILED(hr)) {
      *recognizer_failed = true;
      return Log("ISpRecognizer::SetRecognizer", hr);
    }

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = static_cast<DWORD>(options_.sample_rate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    Ref<ISpStream> stream;
    hr = CoCreateInstance(kClsidSpStream, nullptr, CLSCTX_INPROC_SERVER, __uuidof(ISpStream),
                          reinterpret_cast<void**>(stream.put()));
    Trace("before SetBaseStream");
    if (SUCCEEDED(hr)) hr = stream->SetBaseStream(pipe_, kSpdfidWaveFormatEx, &format);
    Trace("after SetBaseStream");
    // fAllowFormatChanges: SAPI converts the PCM to the engine's own format.
    Trace("before SetInput");
    if (SUCCEEDED(hr)) hr = (*recognizer)->SetInput(stream.get(), TRUE);
    Trace("after SetInput");
    if (FAILED(hr)) return Log("ISpRecognizer::SetInput", hr);

    Trace("before CreateRecoContext");
    hr = (*recognizer)->CreateRecoContext(context->put());
    Trace("after CreateRecoContext");
    if (FAILED(hr)) return Log("ISpRecognizer::CreateRecoContext", hr);
    Trace("before SetNotifyWin32Event");
    hr = (*context)->SetNotifyWin32Event();
    Trace("after SetNotifyWin32Event");
    if (FAILED(hr)) return Log("ISpRecoContext::SetNotifyWin32Event", hr);
    ULONGLONG interest = Interest(SPEI_SOUND_START) | Interest(SPEI_SOUND_END) |
                         Interest(SPEI_RECOGNITION) | Interest(SPEI_FALSE_RECOGNITION) |
                         Interest(SPEI_END_SR_STREAM);
    if (options_.hypotheses) interest |= Interest(SPEI_HYPOTHESIS);
    Trace("before SetInterest");
    hr = (*context)->SetInterest(interest, interest);
    Trace("after SetInterest");
    if (FAILED(hr)) return Log("ISpRecoContext::SetInterest", hr);
    if (options_.max_alternates > 1) {
      // An engine without alternates keeps answering with one text.
      (*context)->SetMaxAlternates(static_cast<ULONG>(options_.max_alternates));
    }
    Trace("before CreateGrammar");
    hr = (*context)->CreateGrammar(0, grammar->put());
    Trace("after CreateGrammar");
    Trace("before LoadDictation");
    if (SUCCEEDED(hr)) hr = (*grammar)->LoadDictation(nullptr, SPLO_STATIC);
    Trace("after LoadDictation");
    if (FAILED(hr)) {
      *recognizer_failed = true;
      return Log("ISpRecoGrammar::LoadDictation", hr);
    }
    Trace("before SetDictationState ACTIVE");
    hr = (*grammar)->SetDictationState(SPRS_ACTIVE);
    Trace("after SetDictationState ACTIVE");
    Trace("before SetRecoState ACTIVE");
    if (SUCCEEDED(hr)) hr = (*recognizer)->SetRecoState(SPRST_ACTIVE);
    Trace("after SetRecoState ACTIVE");
    if (FAILED(hr)) return Log("activating dictation", hr);
    return S_OK;
  }

  void Run() {
    ComScope com;
    Ref<ISpRecognizer> recognizer;
    Ref<ISpRecoContext> context;
    Ref<ISpRecoGrammar> grammar;
    bool recognizer_failed = false;
    HRESULT hr = com.status();
    Trace("before Setup");
    if (SUCCEEDED(hr)) hr = Setup(&recognizer, &context, &grammar, &recognizer_failed);
    Trace("after Setup");
    Trace("before SetupDone");
    SetupDone(hr, recognizer_failed);
    Trace("after SetupDone");
    Trace("before Pump");
    if (SUCCEEDED(hr)) Pump(context.get());
    Trace("after Pump");
    Trace("before SetRecoState INACTIVE_WITH_PURGE");
    if (recognizer.get() != nullptr) recognizer->SetRecoState(SPRST_INACTIVE_WITH_PURGE);
    Trace("after SetRecoState INACTIVE_WITH_PURGE");
    Trace("before grammar release");
    grammar.Reset();
    Trace("after grammar release");
    Trace("before context release");
    context.Reset();
    Trace("after context release");
    Trace("before recognizer release");
    recognizer.Reset();
    Trace("after recognizer release");
  }

  void Pump(ISpRecoContext* context) {
    HANDLE handles[2] = {stop_, context->GetNotifyEventHandle()};
    for (;;) {
      DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
      if (wait != WAIT_OBJECT_0 + 1) return;  // stopped (or the wait failed)
      SPEVENT event;
      ULONG fetched = 0;
      while (context->GetEvents(1, &event, &fetched) == S_OK && fetched == 1) {
        bool done = Translate(event);
        if (event.elParamType == SPET_LPARAM_IS_OBJECT && event.lParam != 0) {
          reinterpret_cast<IUnknown*>(event.lParam)->Release();
        } else if ((event.elParamType == SPET_LPARAM_IS_POINTER ||
                    event.elParamType == SPET_LPARAM_IS_STRING) &&
                   event.lParam != 0) {
          CoTaskMemFree(reinterpret_cast<void*>(event.lParam));
        } else if (event.elParamType == SPET_LPARAM_IS_TOKEN && event.lParam != 0) {
          reinterpret_cast<IUnknown*>(event.lParam)->Release();
        }
        if (done) return;
      }
    }
  }

  // Returns true at the end of the stream.
  bool Translate(const SPEVENT& event) {
    Event e;
    switch (static_cast<SPEVENTENUM>(event.eEventId)) {
      case SPEI_SOUND_START:
        e.kind = EventKind::kSoundStart;
        break;
      case SPEI_SOUND_END:
        e.kind = EventKind::kSoundEnd;
        break;
      case SPEI_HYPOTHESIS:
      case SPEI_RECOGNITION: {
        auto* result = reinterpret_cast<ISpRecoResult*>(event.lParam);
        if (result == nullptr) return false;
        bool is_final = event.eEventId == SPEI_RECOGNITION;
        ReadResult(result, is_final ? options_.max_alternates : 1, &e);
        if (e.texts.empty()) {
          if (!is_final) return false;
          e.kind = EventKind::kNoMatch;
        } else {
          e.kind = is_final ? EventKind::kRecognition : EventKind::kHypothesis;
        }
        break;
      }
      case SPEI_FALSE_RECOGNITION:
        e.kind = EventKind::kNoMatch;
        break;
      case SPEI_END_SR_STREAM: {
        // lParam: the HRESULT the engine's recognition of the stream ended with.
        HRESULT stream_hr = static_cast<HRESULT>(event.lParam);
        if (FAILED(stream_hr)) {
          e.kind = EventKind::kFailed;
          e.hr = stream_hr;
          Log("recognizing the stream", stream_hr);
        } else {
          e.kind = EventKind::kStreamEnd;
        }
        Queue(std::move(e));
        return true;
      }
      default:
        return false;
    }
    Queue(std::move(e));
    return false;
  }

  const SessionOptions options_;
  const std::wstring token_id_;
  HANDLE stop_ = nullptr;
  PcmPipe* pipe_ = nullptr;
  std::thread thread_;

  std::mutex mutex_;
  std::condition_variable started_;
  std::condition_variable queued_;
  std::deque<Event> events_;
  bool setup_done_ = false;
  HRESULT setup_hr_ = E_FAIL;
  bool recognizer_failed_ = false;
  bool canceled_ = false;
  bool failed_ = false;
};

}  // namespace

HRESULT EnumerateRecognizers(std::vector<Recognizer>* out) {
  out->clear();
  ComScope com;
  if (FAILED(com.status())) return com.status();
  return Enumerate(out);
}

int FindRecognizer(const std::vector<Recognizer>& recognizers, const std::wstring& bcp47) {
  if (bcp47.empty()) return -1;
  for (size_t i = 0; i < recognizers.size(); ++i) {
    for (const std::wstring& tag : recognizers[i].languages) {
      if (SameTag(tag, bcp47)) return static_cast<int>(i);
    }
  }
  // "en": a recognizer of English, the default one first. "en-AU" is not "en-US".
  if (bcp47.find(L'-') != std::wstring::npos) return -1;
  int found = -1;
  for (size_t i = 0; i < recognizers.size(); ++i) {
    for (const std::wstring& tag : recognizers[i].languages) {
      if (!SameTag(PrimaryOf(tag), bcp47)) continue;
      if (recognizers[i].is_default) return static_cast<int>(i);
      if (found < 0) found = static_cast<int>(i);
    }
  }
  return found;
}

HRESULT StartSession(const SessionOptions& options, std::unique_ptr<Session>* out,
                     bool* recognizer_failed) {
  out->reset();
  *recognizer_failed = false;
  if (options.sample_rate < 8000 || options.sample_rate > 48000 || options.max_alternates < 1) {
    return E_INVALIDARG;
  }
  std::vector<Recognizer> recognizers;
  HRESULT hr = EnumerateRecognizers(&recognizers);
  if (FAILED(hr)) return hr;
  int index = FindRecognizer(recognizers, options.language);
  if (index < 0) return kSpErrNotFound;
  SessionOptions bounded = options;
  bounded.max_alternates = (std::min)(options.max_alternates, kMaxAlternates);
  auto session = std::make_unique<SapiSession>(bounded, recognizers[index].token_id);
  hr = session->Start(recognizer_failed);
  if (FAILED(hr)) return hr;
  *out = std::move(session);
  return S_OK;
}

}  // namespace gesher::devices::speech_recognition
