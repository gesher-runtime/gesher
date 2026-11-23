// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Gesher (devices track): Windows.Media.SpeechSynthesis through its ABI (docs/tts.md).
//
// The interfaces below are declared by their ABI: IIDs and method order as
// Microsoft's Windows metadata (Windows.winmd, as microsoft/windows-rs generates it)
// describes them; the semantics are those of the Windows API reference
// (MicrosoftDocs/winrt-api): SpeechSynthesizer.AllVoices, SynthesizeTextToStreamAsync,
// SpeechSynthesizerOptions.SpeakingRate (0.5..6.0), AudioPitch (0.0..2.0),
// IncludeWordBoundaryMetadata, and SpeechCue's StartPositionInInput /
// EndPositionInInput ("the last character in the word") on the "SpeechWord" track.

#include "speech_win.h"

#include <objidl.h>
#include <stdio.h>

#include <atomic>
#include <mutex>

#include "winrt.h"

namespace gesher::devices::speech {

using winrt::HSTRING;
using winrt::IAsyncInfo;
using winrt::IAsyncOperation;
using winrt::IInspectable;
using winrt::IReference;

namespace {

constexpr wchar_t kSynthesizer[] = L"Windows.Media.SpeechSynthesis.SpeechSynthesizer";
constexpr wchar_t kWordTrack[] = L"SpeechWord";
// The largest stream read: about 35 minutes of 16-bit 48 kHz stereo, far above
// TextToSpeech.getMaxSpeechInputLength()'s 4000 characters.
constexpr size_t kMaxStreamBytes = 400u << 20;

struct TimeSpan {
  int64_t Duration;
};

template <typename T>
struct IVectorView : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 index, T* item) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32* size) = 0;
};

// Windows.Media.SpeechSynthesis.IVoiceInformation {B127D6A4-1291-4604-AA9C-83134083352C}.
struct IVoiceInformation : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Id(HSTRING* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Language(HSTRING* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Description(HSTRING* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Gender(int* value) = 0;
};

// IInstalledVoicesStatic {7D526ECC-7533-4C3F-85BE-888C2BAEEBDC}: SpeechSynthesizer's statics.
struct IInstalledVoicesStatic : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_AllVoices(IVectorView<IVoiceInformation*>** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_DefaultVoice(IVoiceInformation** value) = 0;
};
const IID IID_IInstalledVoicesStatic = {
    0x7D526ECC, 0x7533, 0x4C3F, {0x85, 0xBE, 0x88, 0x8C, 0x2B, 0xAE, 0xEB, 0xDC}};

// ISpeechSynthesizer {CE9F7C76-97F4-4CED-AD68-D51C458E45C6}.
struct ISpeechSynthesizer : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE SynthesizeTextToStreamAsync(
      HSTRING text, IAsyncOperation<IInspectable*>** operation) = 0;
  virtual HRESULT STDMETHODCALLTYPE SynthesizeSsmlToStreamAsync(
      HSTRING ssml, IAsyncOperation<IInspectable*>** operation) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_Voice(IVoiceInformation* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Voice(IVoiceInformation** value) = 0;
};
const IID IID_ISpeechSynthesizer = {
    0xCE9F7C76, 0x97F4, 0x4CED, {0xAD, 0x68, 0xD5, 0x1C, 0x45, 0x8E, 0x45, 0xC6}};

struct ISpeechSynthesizerOptions;

// ISpeechSynthesizer2 {A7C5ECB2-4339-4D6A-BBF8-C7A4F1544C2E} (Windows 10 1607).
struct ISpeechSynthesizer2 : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Options(ISpeechSynthesizerOptions** value) = 0;
};
const IID IID_ISpeechSynthesizer2 = {
    0xA7C5ECB2, 0x4339, 0x4D6A, {0xBB, 0xF8, 0xC7, 0xA4, 0xF1, 0x54, 0x4C, 0x2E}};

// ISpeechSynthesizerOptions {A0E23871-CC3D-43C9-91B1-EE185324D83D}, Options' default
// interface.
struct ISpeechSynthesizerOptions : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_IncludeWordBoundaryMetadata(boolean* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_IncludeWordBoundaryMetadata(boolean value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IncludeSentenceBoundaryMetadata(boolean* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_IncludeSentenceBoundaryMetadata(boolean value) = 0;
};

// ISpeechSynthesizerOptions2 {1CBEF60E-119C-4BED-B118-D250C3A25793} (Windows 10 1709).
struct ISpeechSynthesizerOptions2 : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_AudioVolume(double* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_AudioVolume(double value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_SpeakingRate(double* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_SpeakingRate(double value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_AudioPitch(double* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_AudioPitch(double value) = 0;
};
const IID IID_ISpeechSynthesizerOptions2 = {
    0x1CBEF60E, 0x119C, 0x4BED, {0xB1, 0x18, 0xD2, 0x50, 0xC3, 0xA2, 0x57, 0x93}};

// Windows.Media.Core.IMediaCue {C7D15E5D-59DC-431F-A0EE-27744323B36D}.
struct IMediaCue : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE put_StartTime(TimeSpan value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_StartTime(TimeSpan* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_Duration(TimeSpan value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Duration(TimeSpan* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_Id(HSTRING value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Id(HSTRING* value) = 0;
};

// ISpeechCue {AEE254DC-1725-4BAD-8043-A98499B017A2}.
struct ISpeechCue : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_Text(HSTRING value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_StartPositionInInput(IReference<INT32>** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_StartPositionInInput(IReference<INT32>* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_EndPositionInInput(IReference<INT32>** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_EndPositionInInput(IReference<INT32>* value) = 0;
};
const IID IID_ISpeechCue = {
    0xAEE254DC, 0x1725, 0x4BAD, {0x80, 0x43, 0xA9, 0x84, 0x99, 0xB0, 0x17, 0xA2}};

// IMediaTrack {03E1FAFC-C931-491A-B46B-C10EE8C256B7}: the track's Label names its cues.
struct IMediaTrack : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Id(HSTRING* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Language(HSTRING* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_TrackKind(int* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_Label(HSTRING value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Label(HSTRING* value) = 0;
};
const IID IID_IMediaTrack = {
    0x03E1FAFC, 0xC931, 0x491A, {0xB4, 0x6B, 0xC1, 0x0E, 0xE8, 0xC2, 0x56, 0xB7}};

// ITimedMetadataTrack {9E6AED9E-F67A-49A9-B330-CF03B0E9CF07}: up to Cues.
struct ITimedMetadataTrack : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE add_CueEntered(IUnknown* handler, int64_t* token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_CueEntered(int64_t token) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_CueExited(IUnknown* handler, int64_t* token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_CueExited(int64_t token) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_TrackFailed(IUnknown* handler, int64_t* token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_TrackFailed(int64_t token) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Cues(IVectorView<IMediaCue*>** value) = 0;
};

// ITimedMetadataTrackProvider {3B7F2024-F74E-4ADE-93C5-219DA05B6856}, on
// SpeechSynthesisStream since Windows 10 1703.
struct ITimedMetadataTrackProvider : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_TimedMetadataTracks(
      IVectorView<ITimedMetadataTrack*>** value) = 0;
};
const IID IID_ITimedMetadataTrackProvider = {
    0x3B7F2024, 0xF74E, 0x4ADE, {0x93, 0xC5, 0x21, 0x9D, 0xA0, 0x5B, 0x68, 0x56}};

constexpr int kStarted = 0, kCompleted = 1, kCanceledStatus = 2;

void Log(const char* call, HRESULT hr) {
  fprintf(stderr, "gesher speech: %s failed: 0x%08lx\n", call, static_cast<unsigned long>(hr));
}

std::wstring Take(HSTRING s) {
  std::wstring value = winrt::StringOf(s);
  winrt::DeleteString(s);
  return value;
}

// shcore's CreateStreamOverRandomAccessStream (Windows 8): a COM IStream over the
// SpeechSynthesisStream's IRandomAccessStream, found at run time.
HRESULT StreamOver(IUnknown* randomAccess, IStream** out) {
  using Fn = HRESULT(WINAPI*)(IUnknown*, REFIID, void**);
  static Fn fn = [] {
    HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return shcore == nullptr ? nullptr
                             : reinterpret_cast<Fn>(reinterpret_cast<void*>(
                                   GetProcAddress(shcore, "CreateStreamOverRandomAccessStream")));
  }();
  *out = nullptr;
  if (fn == nullptr) return E_NOTIMPL;
  return fn(randomAccess, IID_IStream, reinterpret_cast<void**>(out));
}

HRESULT ReadAll(IInspectable* stream, std::vector<uint8_t>* bytes) {
  IStream* com = nullptr;
  HRESULT hr = StreamOver(stream, &com);
  if (FAILED(hr)) return hr;
  bytes->clear();
  uint8_t chunk[64 * 1024];
  for (;;) {
    ULONG read = 0;
    hr = com->Read(chunk, sizeof(chunk), &read);
    if (FAILED(hr)) break;
    if (read == 0) {
      hr = S_OK;
      break;
    }
    if (bytes->size() + read > kMaxStreamBytes) {
      hr = E_OUTOFMEMORY;
      break;
    }
    bytes->insert(bytes->end(), chunk, chunk + read);
    if (hr == S_FALSE) {  // fewer bytes than asked: the end of the stream
      hr = S_OK;
      break;
    }
  }
  winrt::Release(com);
  return hr;
}

bool ValueOf(IReference<INT32>* ref, INT32* value) {
  if (ref == nullptr) return false;
  bool ok = SUCCEEDED(ref->get_Value(value));
  ref->Release();
  return ok;
}

// The cues of the stream's "SpeechWord" track; none when Windows gave no metadata.
void ReadWords(IInspectable* stream, std::vector<Word>* words) {
  words->clear();
  ITimedMetadataTrackProvider* provider = nullptr;
  if (FAILED(stream->QueryInterface(IID_ITimedMetadataTrackProvider,
                                    reinterpret_cast<void**>(&provider)))) {
    return;
  }
  IVectorView<ITimedMetadataTrack*>* tracks = nullptr;
  UINT32 trackCount = 0;
  if (SUCCEEDED(provider->get_TimedMetadataTracks(&tracks)) && tracks != nullptr) {
    tracks->get_Size(&trackCount);
  }
  for (UINT32 t = 0; t < trackCount; t++) {
    ITimedMetadataTrack* track = nullptr;
    if (FAILED(tracks->GetAt(t, &track)) || track == nullptr) continue;
    IMediaTrack* media = nullptr;
    std::wstring label;
    if (SUCCEEDED(track->QueryInterface(IID_IMediaTrack, reinterpret_cast<void**>(&media)))) {
      HSTRING s = nullptr;
      if (SUCCEEDED(media->get_Label(&s))) label = Take(s);
      winrt::Release(media);
    }
    IVectorView<IMediaCue*>* cues = nullptr;
    UINT32 cueCount = 0;
    if (label == kWordTrack && SUCCEEDED(track->get_Cues(&cues)) && cues != nullptr) {
      cues->get_Size(&cueCount);
    }
    for (UINT32 c = 0; c < cueCount; c++) {
      IMediaCue* cue = nullptr;
      if (FAILED(cues->GetAt(c, &cue)) || cue == nullptr) continue;
      TimeSpan start = {0};
      ISpeechCue* speech = nullptr;
      IReference<INT32>* first = nullptr;
      IReference<INT32>* last = nullptr;
      Word word;
      if (SUCCEEDED(cue->get_StartTime(&start)) &&
          SUCCEEDED(cue->QueryInterface(IID_ISpeechCue, reinterpret_cast<void**>(&speech)))) {
        speech->get_StartPositionInInput(&first);
        speech->get_EndPositionInInput(&last);
        bool hasFirst = ValueOf(first, &word.first);
        bool hasLast = ValueOf(last, &word.last);
        if (hasFirst && hasLast) {
          word.startTicks = start.Duration;
          words->push_back(word);
        }
      }
      winrt::Release(speech);
      winrt::Release(cue);
    }
    winrt::Release(cues);
    winrt::Release(track);
  }
  winrt::Release(tracks);
  winrt::Release(provider);
}

}  // namespace

struct Synthesis {
  ISpeechSynthesizer* synthesizer = nullptr;
  IAsyncOperation<IInspectable*>* operation = nullptr;
  IAsyncInfo* info = nullptr;
  bool words = false;
  std::vector<uint8_t> audio;
  std::vector<Word> wordList;
  std::mutex lock;
  std::atomic<bool> cancel{false};
};

bool Voices(std::vector<Voice>* voices, HRESULT* hr) {
  voices->clear();
  *hr = S_OK;
  IInstalledVoicesStatic* statics = nullptr;
  HRESULT r = winrt::GetActivationFactory(kSynthesizer, IID_IInstalledVoicesStatic,
                                          reinterpret_cast<void**>(&statics));
  if (FAILED(r)) {
    Log("GetActivationFactory(SpeechSynthesizer)", r);
    *hr = r;
    return false;
  }
  std::wstring defaultId;
  IVoiceInformation* fallback = nullptr;
  if (SUCCEEDED(statics->get_DefaultVoice(&fallback)) && fallback != nullptr) {
    HSTRING s = nullptr;
    if (SUCCEEDED(fallback->get_Id(&s))) defaultId = Take(s);
    winrt::Release(fallback);
  }
  IVectorView<IVoiceInformation*>* all = nullptr;
  r = statics->get_AllVoices(&all);
  winrt::Release(statics);
  if (FAILED(r) || all == nullptr) {
    Log("SpeechSynthesizer.AllVoices", r);
    *hr = FAILED(r) ? r : E_POINTER;
    return false;
  }
  UINT32 count = 0;
  all->get_Size(&count);
  for (UINT32 i = 0; i < count; i++) {
    IVoiceInformation* info = nullptr;
    if (FAILED(all->GetAt(i, &info)) || info == nullptr) continue;
    Voice v;
    HSTRING s = nullptr;
    if (SUCCEEDED(info->get_Id(&s))) v.id = Take(s);
    s = nullptr;
    if (SUCCEEDED(info->get_DisplayName(&s))) v.displayName = Take(s);
    s = nullptr;
    if (SUCCEEDED(info->get_Language(&s))) v.language = Take(s);
    if (FAILED(info->get_Gender(&v.gender))) v.gender = 0;
    winrt::Release(info);
    // A voice is selected by its Id and spoken in its language: without both it
    // cannot be offered.
    if (v.id.empty() || v.language.empty()) continue;
    v.isDefault = v.id == defaultId;
    voices->push_back(v);
  }
  winrt::Release(all);
  return true;
}

namespace {

// The VoiceInformation of `voiceId` among AllVoices, or null.
IVoiceInformation* FindVoice(const wchar_t* voiceId, HRESULT* hr) {
  IInstalledVoicesStatic* statics = nullptr;
  *hr = winrt::GetActivationFactory(kSynthesizer, IID_IInstalledVoicesStatic,
                                    reinterpret_cast<void**>(&statics));
  if (FAILED(*hr)) return nullptr;
  IVectorView<IVoiceInformation*>* all = nullptr;
  *hr = statics->get_AllVoices(&all);
  winrt::Release(statics);
  if (FAILED(*hr) || all == nullptr) return nullptr;
  UINT32 count = 0;
  all->get_Size(&count);
  IVoiceInformation* found = nullptr;
  for (UINT32 i = 0; i < count && found == nullptr; i++) {
    IVoiceInformation* info = nullptr;
    if (FAILED(all->GetAt(i, &info)) || info == nullptr) continue;
    HSTRING s = nullptr;
    if (SUCCEEDED(info->get_Id(&s)) && Take(s) == voiceId) {
      found = info;
    } else {
      winrt::Release(info);
    }
  }
  winrt::Release(all);
  return found;
}

void Close(IUnknown* object) {
  winrt::IClosable* closable = nullptr;
  if (object != nullptr &&
      SUCCEEDED(object->QueryInterface(winrt::IID_IClosable, reinterpret_cast<void**>(&closable)))) {
    closable->Close();
    closable->Release();
  }
}

}  // namespace

Synthesis* Start(const wchar_t* text, size_t length, const wchar_t* voiceId, double rate,
                 double pitch, bool words, StartFailure* failure, HRESULT* hr) {
  *failure = StartFailure::kWindows;
  *hr = winrt::EnsureApartment();
  if (FAILED(*hr)) {
    Log("RoInitialize", *hr);
    return nullptr;
  }
  if (length > 0xFFFFFFFFu) {
    *hr = E_INVALIDARG;
    return nullptr;
  }
  IVoiceInformation* voice = FindVoice(voiceId, hr);
  if (voice == nullptr) {
    if (SUCCEEDED(*hr)) *failure = StartFailure::kNoSuchVoice;
    else Log("SpeechSynthesizer.AllVoices", *hr);
    return nullptr;
  }
  IInspectable* instance = nullptr;
  ISpeechSynthesizer* synthesizer = nullptr;
  *hr = winrt::ActivateInstance(kSynthesizer, &instance);
  if (SUCCEEDED(*hr)) {
    *hr = instance->QueryInterface(IID_ISpeechSynthesizer, reinterpret_cast<void**>(&synthesizer));
  }
  winrt::Release(instance);
  if (SUCCEEDED(*hr)) *hr = synthesizer->put_Voice(voice);
  winrt::Release(voice);
  if (FAILED(*hr)) {
    Log("SpeechSynthesizer(Voice)", *hr);
    Close(synthesizer);
    winrt::Release(synthesizer);
    return nullptr;
  }

  // Options: word boundaries (ISpeechSynthesizerOptions) and prosody (Options2).
  ISpeechSynthesizer2* synthesizer2 = nullptr;
  ISpeechSynthesizerOptions* options = nullptr;
  ISpeechSynthesizerOptions2* options2 = nullptr;
  if (SUCCEEDED(synthesizer->QueryInterface(IID_ISpeechSynthesizer2,
                                            reinterpret_cast<void**>(&synthesizer2)))) {
    synthesizer2->get_Options(&options);
    winrt::Release(synthesizer2);
  }
  if (options != nullptr) {
    options->QueryInterface(IID_ISpeechSynthesizerOptions2, reinterpret_cast<void**>(&options2));
  }
  bool prosody = rate != 1.0 || pitch != 1.0;
  HRESULT r = S_OK;
  if (options2 != nullptr) {
    r = options2->put_SpeakingRate(rate);
    if (SUCCEEDED(r)) r = options2->put_AudioPitch(pitch);
  } else if (prosody) {
    // Windows before 10 1709: the requested rate or pitch cannot be applied.
    *failure = StartFailure::kProsodyUnsupported;
    r = E_NOINTERFACE;
  }
  bool wantWords = words && options != nullptr;
  if (SUCCEEDED(r) && wantWords) r = options->put_IncludeWordBoundaryMetadata(TRUE);
  winrt::Release(options2);
  winrt::Release(options);
  if (FAILED(r)) {
    if (*failure != StartFailure::kProsodyUnsupported) Log("SpeechSynthesizerOptions", r);
    *hr = r;
    Close(synthesizer);
    winrt::Release(synthesizer);
    return nullptr;
  }

  HSTRING input = nullptr;
  *hr = winrt::CreateString(text, static_cast<UINT32>(length), &input);
  IAsyncOperation<IInspectable*>* operation = nullptr;
  if (SUCCEEDED(*hr)) *hr = synthesizer->SynthesizeTextToStreamAsync(input, &operation);
  winrt::DeleteString(input);
  IAsyncInfo* info = nullptr;
  if (SUCCEEDED(*hr)) {
    *hr = operation->QueryInterface(winrt::IID_IAsyncInfo, reinterpret_cast<void**>(&info));
  }
  if (FAILED(*hr)) {
    Log("SynthesizeTextToStreamAsync", *hr);
    winrt::Release(operation);
    Close(synthesizer);
    winrt::Release(synthesizer);
    return nullptr;
  }
  Synthesis* s = new Synthesis;
  s->synthesizer = synthesizer;
  s->operation = operation;
  s->info = info;
  s->words = wantWords;
  *failure = StartFailure::kNone;
  return s;
}

int Await(Synthesis* s, int64_t timeoutMs, HRESULT* hr) {
  *hr = S_OK;
  ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs < 0 ? 0 : timeoutMs);
  int status = kStarted;
  DWORD pause = 1;
  HRESULT r;
  while (SUCCEEDED(r = s->info->get_Status(&status)) && status == kStarted) {
    if (GetTickCount64() >= deadline) {
      Cancel(s);
      *hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
      return kFailed;
    }
    Sleep(pause);
    if (pause < 10) pause *= 2;
  }
  if (FAILED(r)) {
    Log("IAsyncInfo::get_Status", r);
    *hr = r;
    return kFailed;
  }
  if (status == kCanceledStatus || s->cancel.load()) return kCanceled;
  if (status != kCompleted) {
    HRESULT code = E_FAIL;
    s->info->get_ErrorCode(&code);
    Log("SynthesizeTextToStreamAsync completion", code);
    *hr = FAILED(code) ? code : E_FAIL;
    return kFailed;
  }
  IInspectable* stream = nullptr;
  r = s->operation->GetResults(&stream);
  if (FAILED(r) || stream == nullptr) {
    Log("GetResults(SpeechSynthesisStream)", r);
    *hr = FAILED(r) ? r : E_POINTER;
    return kFailed;
  }
  r = ReadAll(stream, &s->audio);
  if (SUCCEEDED(r) && s->words) ReadWords(stream, &s->wordList);
  Close(stream);
  winrt::Release(stream);
  if (FAILED(r)) {
    Log("reading the SpeechSynthesisStream", r);
    *hr = r;
    return kFailed;
  }
  return s->cancel.load() ? kCanceled : kDone;
}

const std::vector<uint8_t>& Audio(const Synthesis* s) { return s->audio; }

const std::vector<Word>& Words(const Synthesis* s) { return s->wordList; }

void Cancel(Synthesis* s) {
  std::lock_guard<std::mutex> guard(s->lock);
  if (s->cancel.exchange(true)) return;
  HRESULT r = s->info->Cancel();
  if (FAILED(r)) Log("IAsyncInfo::Cancel", r);
}

void Release(Synthesis* s) {
  {
    std::lock_guard<std::mutex> guard(s->lock);
    s->info->Close();
    winrt::Release(s->info);
    winrt::Release(s->operation);
    Close(s->synthesizer);
    winrt::Release(s->synthesizer);
  }
  delete s;
}

}  // namespace gesher::devices::speech
