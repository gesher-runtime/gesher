// Gesher (devices track): cameras over Media Foundation, and the
// GESHER_CAMERA_TEST camera (camera_mf.h).

// Media Foundation's headers use GDI's types.
#undef NOGDI
#include "camera_mf.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <mutex>

namespace gesher::devices::camera {

namespace {

// The GUIDs used, as the Windows SDK defines them (not taken from mfuuid).
constexpr GUID kDevSourceType = {0xc60ac5fe, 0x252a, 0x478f,
                                 {0xa0, 0xef, 0xbc, 0x8f, 0xa5, 0xf7, 0xca, 0xd3}};
constexpr GUID kDevSourceVidcap = {0x8ac3587a, 0x4ae7, 0x42d8,
                                   {0x99, 0xe0, 0x0a, 0x60, 0x13, 0xee, 0xf9, 0x0f}};
constexpr GUID kDevSourceFriendlyName = {0x60d0e559, 0x52f8, 0x4fa2,
                                         {0xbb, 0xce, 0xac, 0xdb, 0x34, 0xa8, 0xec, 0x01}};
constexpr GUID kDevSourceSymbolicLink = {0x58f0aad8, 0x22bf, 0x4f8a,
                                         {0xbb, 0x3d, 0xd2, 0xc4, 0x97, 0x8c, 0x6e, 0x2f}};
// MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING: conversions between YUV
// formats (YUY2 to NV12, as webcams offer YUY2) and resizing, not only to RGB32.
constexpr GUID kReaderVideoProcessing = {0x0f81da2c, 0xb537, 0x4672,
                                         {0xa8, 0xb2, 0xa6, 0x81, 0xb1, 0x73, 0x07, 0xa3}};
constexpr GUID kMtMajorType = {0x48eba18e, 0xf8c9, 0x4687,
                               {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
constexpr GUID kMtSubtype = {0xf7e34c9a, 0x42e8, 0x4714,
                             {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
constexpr GUID kMtFrameSize = {0x1652c33d, 0xd6b2, 0x4012,
                               {0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d}};
constexpr GUID kMediaTypeVideo = {0x73646976, 0x0000, 0x0010,
                                  {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kVideoFormatNv12 = {0x3231564e, 0x0000, 0x0010,
                                   {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr DWORD kFirstVideoStream = 0xFFFFFFFC;  // MF_SOURCE_READER_FIRST_VIDEO_STREAM
constexpr ULONG kMfVersion = 0x00020070;         // MF_VERSION
constexpr DWORD kStartupLite = 1;                // MFSTARTUP_LITE

constexpr char kTestId[] = "gesher-test";

struct Api {
  HRESULT(WINAPI* MFStartup)(ULONG version, DWORD flags) = nullptr;
  HRESULT(WINAPI* MFCreateAttributes)(IMFAttributes** attributes, UINT32 size) = nullptr;
  HRESULT(WINAPI* MFCreateMediaType)(IMFMediaType** type) = nullptr;
  HRESULT(WINAPI* MFEnumDeviceSources)(IMFAttributes* attributes, IMFActivate*** sources,
                                       UINT32* count) = nullptr;
  HRESULT(WINAPI* MFCreateSourceReaderFromMediaSource)(IMFMediaSource* source,
                                                       IMFAttributes* attributes,
                                                       IMFSourceReader** reader) = nullptr;
  // The test camera's media source (TestSource).
  HRESULT(WINAPI* MFCreateEventQueue)(IMFMediaEventQueue** queue) = nullptr;
  HRESULT(WINAPI* MFCreateStreamDescriptor)(DWORD id, DWORD count, IMFMediaType** types,
                                            IMFStreamDescriptor** descriptor) = nullptr;
  HRESULT(WINAPI* MFCreatePresentationDescriptor)(DWORD count, IMFStreamDescriptor** streams,
                                                  IMFPresentationDescriptor** pd) = nullptr;
  HRESULT(WINAPI* MFCreateSample)(IMFSample** sample) = nullptr;
  HRESULT(WINAPI* MFCreateMemoryBuffer)(DWORD max, IMFMediaBuffer** buffer) = nullptr;
  HRESULT startup = E_NOTIMPL;
  std::string missing;
};

template <typename F>
void Find(HMODULE module, const char* name, F& f, std::string* missing) {
  f = module != nullptr ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(module, name)))
                        : nullptr;
  if (f == nullptr) {
    if (!missing->empty()) *missing += ", ";
    *missing += name;
  }
}

const Api& GetApi() {
  static Api api;
  static std::once_flag once;
  std::call_once(once, [] {
    HMODULE mfplat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE mf = LoadLibraryExW(L"mf.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE mfreadwrite = LoadLibraryExW(L"mfreadwrite.dll", nullptr,
                                         LOAD_LIBRARY_SEARCH_SYSTEM32);
    Find(mfplat, "MFStartup", api.MFStartup, &api.missing);
    Find(mfplat, "MFCreateAttributes", api.MFCreateAttributes, &api.missing);
    Find(mfplat, "MFCreateMediaType", api.MFCreateMediaType, &api.missing);
    Find(mf, "MFEnumDeviceSources", api.MFEnumDeviceSources, &api.missing);
    Find(mfreadwrite, "MFCreateSourceReaderFromMediaSource",
         api.MFCreateSourceReaderFromMediaSource, &api.missing);
    Find(mfplat, "MFCreateEventQueue", api.MFCreateEventQueue, &api.missing);
    Find(mfplat, "MFCreateStreamDescriptor", api.MFCreateStreamDescriptor, &api.missing);
    Find(mfplat, "MFCreatePresentationDescriptor", api.MFCreatePresentationDescriptor,
         &api.missing);
    Find(mfplat, "MFCreateSample", api.MFCreateSample, &api.missing);
    Find(mfplat, "MFCreateMemoryBuffer", api.MFCreateMemoryBuffer, &api.missing);
    if (api.missing.empty()) api.startup = api.MFStartup(kMfVersion, kStartupLite);
  });
  return api;
}

// COM on this thread (multithreaded), once.
void EnsureCom() {
  thread_local bool done = [] {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return true;
  }();
  (void)done;
}

template <typename T>
void SafeRelease(T*& p) {
  if (p != nullptr) {
    p->Release();
    p = nullptr;
  }
}

std::string Utf8(const wchar_t* s) {
  if (s == nullptr) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  std::string out(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
  return out;
}

std::string Hr(HRESULT hr) {
  char buf[32];
  snprintf(buf, sizeof(buf), "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
  return buf;
}

// The capture devices' activation objects (the caller releases them and the array).
HRESULT EnumDevices(IMFActivate*** devices, UINT32* count) {
  const Api& api = GetApi();
  *devices = nullptr;
  *count = 0;
  if (!api.missing.empty()) return E_NOTIMPL;
  if (FAILED(api.startup)) return api.startup;
  EnsureCom();
  IMFAttributes* attributes = nullptr;
  HRESULT hr = api.MFCreateAttributes(&attributes, 1);
  if (SUCCEEDED(hr)) hr = attributes->SetGUID(kDevSourceType, kDevSourceVidcap);
  if (SUCCEEDED(hr)) hr = api.MFEnumDeviceSources(attributes, devices, count);
  SafeRelease(attributes);
  return hr;
}

void FreeDevices(IMFActivate** devices, UINT32 count) {
  for (UINT32 i = 0; i < count; ++i) SafeRelease(devices[i]);
  CoTaskMemFree(devices);
}

// A source reader over a media source, with video processing (NV12 out); on
// failure the source is shut down and released.
HRESULT NewReaderFor(IMFMediaSource** source, IMFSourceReader** reader) {
  const Api& api = GetApi();
  *reader = nullptr;
  IMFAttributes* attributes = nullptr;
  HRESULT hr = api.MFCreateAttributes(&attributes, 1);
  if (SUCCEEDED(hr)) hr = attributes->SetUINT32(kReaderVideoProcessing, TRUE);
  if (SUCCEEDED(hr)) hr = api.MFCreateSourceReaderFromMediaSource(*source, attributes, reader);
  SafeRelease(attributes);
  if (FAILED(hr)) {
    (*source)->Shutdown();
    SafeRelease(*source);
  }
  return hr;
}

// A source reader over a device's media source.
HRESULT NewReader(IMFActivate* device, IMFMediaSource** source, IMFSourceReader** reader) {
  *source = nullptr;
  *reader = nullptr;
  HRESULT hr = device->ActivateObject(__uuidof(IMFMediaSource), reinterpret_cast<void**>(source));
  if (FAILED(hr)) return hr;
  return NewReaderFor(source, reader);
}

std::vector<std::pair<int, int>> NativeSizes(IMFSourceReader* reader) {
  std::vector<std::pair<int, int>> sizes;
  for (DWORD i = 0;; ++i) {
    IMFMediaType* type = nullptr;
    if (FAILED(reader->GetNativeMediaType(kFirstVideoStream, i, &type))) break;
    UINT64 packed = 0;
    if (SUCCEEDED(type->GetUINT64(kMtFrameSize, &packed))) {
      std::pair<int, int> size(static_cast<int>(packed >> 32), static_cast<int>(packed & 0xFFFFFFFF));
      if (size.first > 0 && size.second > 0 && size.first % 2 == 0 && size.second % 2 == 0 &&
          std::find(sizes.begin(), sizes.end(), size) == sizes.end()) {
        sizes.push_back(size);
      }
    }
    SafeRelease(type);
  }
  std::sort(sizes.begin(), sizes.end(), [](const auto& a, const auto& b) {
    return a.first * a.second > b.first * b.second;
  });
  return sizes;
}

bool TestCameraWanted() {
  const char* test = getenv("GESHER_CAMERA_TEST");
  return test != nullptr && *test != '\0' && *test != '0';
}

// --- the test camera's picture -----------------------------------------------------------

// Frame `frame` of the test picture as YUY2 (Y0 U Y1 V): Y = x + y, gray chroma,
// with a bright 32-pixel square moving across rows 0..31, so frames differ
// while row 100 stays known.
void TestPictureYuy2(uint8_t* out, int width, int height, int frame) {
  int square = (frame * 8) % std::max(width - 32, 1);
  for (int y = 0; y < height; ++y) {
    uint8_t* row = out + static_cast<size_t>(y) * width * 2;
    for (int x = 0; x < width; ++x) {
      bool bright = y < 32 && x >= square && x < square + 32;
      row[x * 2] = bright ? 235 : static_cast<uint8_t>(x + y);
      row[x * 2 + 1] = 128;
    }
  }
}

// The same picture as NV12 (the frames of TestCapture, without Media Foundation).
void TestPictureNv12(uint8_t* nv12, int width, int height, int frame) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) nv12[y * width + x] = static_cast<uint8_t>(x + y);
  }
  int square = (frame * 8) % std::max(width - 32, 1);
  for (int y = 0; y < 32 && y < height; ++y) {
    memset(nv12 + y * width + square, 235, std::min(32, width - square));
  }
  memset(nv12 + static_cast<size_t>(width) * height, 128, static_cast<size_t>(width) * height / 2);
}

// --- the test camera as a Media Foundation source ----------------------------------------
//
// A live IMFMediaSource with one video stream offering YUY2 at 640x480 and
// 320x240 (as many webcams offer YUY2), read like a capture device: the same
// IMFSourceReader, its video processing (YUY2 to NV12) and MfCapture's sample
// copying as the PC's cameras. What it does not exercise: device enumeration
// (MFEnumDeviceSources finds no device to activate on the CI runners) and the
// Frame Server / driver below a real camera's source.

constexpr GUID kMtFrameRate = {0xc459a2e8, 0x3d2c, 0x4e44,
                               {0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0}};
constexpr GUID kMtInterlaceMode = {0xe2724bb8, 0xe676, 0x4806,
                                   {0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0xb4, 0x4c, 0xcd}};
constexpr GUID kMtDefaultStride = {0x644b4e48, 0x1e02, 0x4516,
                                   {0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6}};
constexpr GUID kMtAllSamplesIndependent = {0xc9173739, 0x5e56, 0x461c,
                                           {0xb7, 0x13, 0x46, 0xfb, 0x99, 0x5c, 0xb9, 0x5f}};
constexpr GUID kMtPixelAspectRatio = {0xc6376a1e, 0x8d0a, 0x4027,
                                      {0xbe, 0x45, 0x6d, 0x9a, 0x0a, 0xd3, 0x9b, 0xb6}};
constexpr GUID kVideoFormatYuy2 = {0x32595559, 0x0000, 0x0010,
                                   {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kSampleToken = {0x8294da66, 0xf328, 0x4805,
                               {0xb5, 0x51, 0x00, 0xde, 0xb4, 0xc5, 0x7a, 0x61}};
constexpr HRESULT kShutdown = static_cast<HRESULT>(0xC00D3E85);  // MF_E_SHUTDOWN
constexpr LONGLONG kFrameTime = 333333;                           // 100 ns units, 30 a second

HRESULT NewYuy2Type(int width, int height, IMFMediaType** out) {
  IMFMediaType* type = nullptr;
  HRESULT hr = GetApi().MFCreateMediaType(&type);
  if (SUCCEEDED(hr)) hr = type->SetGUID(kMtMajorType, kMediaTypeVideo);
  if (SUCCEEDED(hr)) hr = type->SetGUID(kMtSubtype, kVideoFormatYuy2);
  if (SUCCEEDED(hr)) {
    hr = type->SetUINT64(kMtFrameSize, (static_cast<UINT64>(width) << 32) | height);
  }
  if (SUCCEEDED(hr)) hr = type->SetUINT64(kMtFrameRate, (UINT64{30} << 32) | 1);
  if (SUCCEEDED(hr)) hr = type->SetUINT64(kMtPixelAspectRatio, (UINT64{1} << 32) | 1);
  if (SUCCEEDED(hr)) hr = type->SetUINT32(kMtInterlaceMode, 2);  // MFVideoInterlace_Progressive
  if (SUCCEEDED(hr)) hr = type->SetUINT32(kMtDefaultStride, static_cast<UINT32>(width * 2));
  if (SUCCEEDED(hr)) hr = type->SetUINT32(kMtAllSamplesIndependent, TRUE);
  if (FAILED(hr)) {
    SafeRelease(type);
    return hr;
  }
  *out = type;
  return S_OK;
}

class TestSource;

class TestStream final : public IMFMediaStream {
 public:
  TestStream(TestSource* source, IMFStreamDescriptor* sd, IMFMediaEventQueue* queue)
      : source_(source), sd_(sd), queue_(queue) {
    sd_->AddRef();
  }

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEventGenerator) ||
        iid == __uuidof(IMFMediaStream)) {
      *out = static_cast<IMFMediaStream*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return n;
  }

  // IMFMediaEventGenerator
  STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override {
    IMFMediaEventQueue* queue = Queue();
    if (queue == nullptr) return kShutdown;
    HRESULT hr = queue->GetEvent(flags, event);
    queue->Release();
    return hr;
  }
  STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_ != nullptr ? queue_->BeginGetEvent(callback, state) : kShutdown;
  }
  STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_ != nullptr ? queue_->EndGetEvent(result, event) : kShutdown;
  }
  STDMETHODIMP QueueEvent(MediaEventType type, REFGUID ext, HRESULT status,
                          const PROPVARIANT* value) override {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_ != nullptr ? queue_->QueueEventParamVar(type, ext, status, value) : kShutdown;
  }

  // IMFMediaStream
  STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
  STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** sd) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_ == nullptr) return kShutdown;
    *sd = sd_;
    sd_->AddRef();
    return S_OK;
  }
  STDMETHODIMP RequestSample(IUnknown* token) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_ == nullptr) return kShutdown;
    if (!started_) return static_cast<HRESULT>(0xC00D36B2);  // MF_E_INVALIDREQUEST
    // The current type (the reader picks one of the offered sizes).
    int width = 640, height = 480;
    IMFMediaTypeHandler* handler = nullptr;
    if (SUCCEEDED(sd_->GetMediaTypeHandler(&handler))) {
      IMFMediaType* type = nullptr;
      if (SUCCEEDED(handler->GetCurrentMediaType(&type))) {
        UINT64 packed = 0;
        if (SUCCEEDED(type->GetUINT64(kMtFrameSize, &packed))) {
          width = static_cast<int>(packed >> 32);
          height = static_cast<int>(packed & 0xFFFFFFFF);
        }
        type->Release();
      }
      handler->Release();
    }
    const Api& api = GetApi();
    DWORD bytes = static_cast<DWORD>(width) * height * 2;
    IMFMediaBuffer* buffer = nullptr;
    IMFSample* sample = nullptr;
    HRESULT hr = api.MFCreateMemoryBuffer(bytes, &buffer);
    BYTE* data = nullptr;
    if (SUCCEEDED(hr)) hr = buffer->Lock(&data, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
      TestPictureYuy2(data, width, height, frame_++);
      buffer->Unlock();
      hr = buffer->SetCurrentLength(bytes);
    }
    if (SUCCEEDED(hr)) hr = api.MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(time_);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(kFrameTime);
    if (SUCCEEDED(hr) && token != nullptr) hr = sample->SetUnknown(kSampleToken, token);
    time_ += kFrameTime;
    if (SUCCEEDED(hr)) hr = queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample);
    SafeRelease(sample);
    SafeRelease(buffer);
    return hr;
  }

  // From the source.
  HRESULT Started(const PROPVARIANT* start) {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_ == nullptr) return kShutdown;
    started_ = true;
    return queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, start);
  }
  HRESULT Stopped() {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_ == nullptr) return kShutdown;
    started_ = false;
    return queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
  }
  void Shutdown() {
    IMFMediaEventQueue* queue;
    {
      std::lock_guard<std::mutex> lock(mu_);
      queue = queue_;
      queue_ = nullptr;
      source_ = nullptr;
    }
    if (queue != nullptr) {
      queue->Shutdown();
      queue->Release();
    }
  }

 private:
  ~TestStream() { SafeRelease(sd_); }
  IMFMediaEventQueue* Queue() {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_ != nullptr) queue_->AddRef();
    return queue_;
  }

  LONG refs_ = 1;
  std::mutex mu_;
  TestSource* source_;  // not a reference: the source owns the stream
  IMFStreamDescriptor* sd_;
  IMFMediaEventQueue* queue_;
  bool started_ = false;
  int frame_ = 0;
  LONGLONG time_ = 0;
};

class TestSource final : public IMFMediaSource {
 public:
  static HRESULT Create(IMFMediaSource** out) {
    const Api& api = GetApi();
    *out = nullptr;
    IMFMediaType* types[2] = {nullptr, nullptr};
    IMFStreamDescriptor* sd = nullptr;
    IMFPresentationDescriptor* pd = nullptr;
    IMFMediaEventQueue* queue = nullptr;
    IMFMediaEventQueue* streamQueue = nullptr;
    IMFMediaTypeHandler* handler = nullptr;
    HRESULT hr = NewYuy2Type(640, 480, &types[0]);
    if (SUCCEEDED(hr)) hr = NewYuy2Type(320, 240, &types[1]);
    if (SUCCEEDED(hr)) hr = api.MFCreateStreamDescriptor(0, 2, types, &sd);
    if (SUCCEEDED(hr)) hr = sd->GetMediaTypeHandler(&handler);
    if (SUCCEEDED(hr)) hr = handler->SetCurrentMediaType(types[0]);
    if (SUCCEEDED(hr)) hr = api.MFCreatePresentationDescriptor(1, &sd, &pd);
    if (SUCCEEDED(hr)) hr = pd->SelectStream(0);
    if (SUCCEEDED(hr)) hr = api.MFCreateEventQueue(&queue);
    if (SUCCEEDED(hr)) hr = api.MFCreateEventQueue(&streamQueue);
    if (SUCCEEDED(hr)) {
      TestSource* source = new TestSource(pd, queue);
      source->stream_ = new TestStream(source, sd, streamQueue);
      pd = nullptr;
      queue = streamQueue = nullptr;
      *out = source;
    }
    SafeRelease(handler);
    SafeRelease(queue);
    SafeRelease(streamQueue);
    SafeRelease(pd);
    SafeRelease(sd);
    SafeRelease(types[0]);
    SafeRelease(types[1]);
    return hr;
  }

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEventGenerator) ||
        iid == __uuidof(IMFMediaSource)) {
      *out = static_cast<IMFMediaSource*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return n;
  }

  // IMFMediaEventGenerator
  STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override {
    IMFMediaEventQueue* queue = nullptr;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (queue_ == nullptr) return kShutdown;
      queue = queue_;
      queue->AddRef();
    }
    HRESULT hr = queue->GetEvent(flags, event);  // may block: not under the lock
    queue->Release();
    return hr;
  }
  STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_ != nullptr ? queue_->BeginGetEvent(callback, state) : kShutdown;
  }
  STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_ != nullptr ? queue_->EndGetEvent(result, event) : kShutdown;
  }
  STDMETHODIMP QueueEvent(MediaEventType type, REFGUID ext, HRESULT status,
                          const PROPVARIANT* value) override {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_ != nullptr ? queue_->QueueEventParamVar(type, ext, status, value) : kShutdown;
  }

  // IMFMediaSource
  STDMETHODIMP GetCharacteristics(DWORD* characteristics) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_ == nullptr) return kShutdown;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
  }
  STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_ == nullptr) return kShutdown;
    return pd_->Clone(pd);
  }
  STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* timeFormat,
                     const PROPVARIANT* start) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_ == nullptr) return kShutdown;
    if (timeFormat != nullptr && *timeFormat != GUID_NULL) {
      return static_cast<HRESULT>(0xC00D3E89);  // MF_E_UNSUPPORTED_TIME_FORMAT
    }
    BOOL selected = FALSE;
    IMFStreamDescriptor* sd = nullptr;
    HRESULT hr = pd != nullptr ? pd->GetStreamDescriptorByIndex(0, &selected, &sd) : E_INVALIDARG;
    SafeRelease(sd);
    if (FAILED(hr)) return hr;
    if (selected) {
      hr = queue_->QueueEventParamUnk(announced_ ? MEUpdatedStream : MENewStream, GUID_NULL,
                                      S_OK, stream_);
      announced_ = true;
      if (SUCCEEDED(hr)) hr = stream_->Started(start);
    }
    if (SUCCEEDED(hr)) hr = queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, start);
    return hr;
  }
  STDMETHODIMP Stop() override {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_ == nullptr) return kShutdown;
    HRESULT hr = stream_->Stopped();
    if (SUCCEEDED(hr)) hr = queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
    return hr;
  }
  STDMETHODIMP Pause() override {
    // A live source does not pause (as cameras' sources).
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_ == nullptr) return kShutdown;
    return static_cast<HRESULT>(0xC00D3E82);  // MF_E_INVALID_STATE_TRANSITION
  }
  STDMETHODIMP Shutdown() override {
    IMFMediaEventQueue* queue;
    TestStream* stream;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (queue_ == nullptr) return kShutdown;
      queue = queue_;
      queue_ = nullptr;
      stream = stream_;
      stream_ = nullptr;
    }
    stream->Shutdown();
    stream->Release();
    queue->Shutdown();
    queue->Release();
    return S_OK;
  }

 private:
  TestSource(IMFPresentationDescriptor* pd, IMFMediaEventQueue* queue) : pd_(pd), queue_(queue) {}
  ~TestSource() {
    if (stream_ != nullptr) {
      stream_->Shutdown();
      stream_->Release();
    }
    SafeRelease(queue_);
    SafeRelease(pd_);
  }

  LONG refs_ = 1;
  std::mutex mu_;
  IMFPresentationDescriptor* pd_;
  IMFMediaEventQueue* queue_;
  TestStream* stream_ = nullptr;
  bool announced_ = false;
};

STDMETHODIMP TestStream::GetMediaSource(IMFMediaSource** source) {
  std::lock_guard<std::mutex> lock(mu_);
  if (queue_ == nullptr || source_ == nullptr) return kShutdown;
  *source = source_;
  source_->AddRef();
  return S_OK;
}

// A source reader over the test camera's media source; with `why` what failed.
HRESULT NewTestReader(IMFMediaSource** source, IMFSourceReader** reader) {
  *source = nullptr;
  *reader = nullptr;
  const Api& api = GetApi();
  if (!api.missing.empty()) return E_NOTIMPL;
  if (FAILED(api.startup)) return api.startup;
  EnsureCom();
  HRESULT hr = TestSource::Create(source);
  if (FAILED(hr)) return hr;
  return NewReaderFor(source, reader);
}

std::vector<Device> FindDevices(std::string* status) {
  std::vector<Device> list;
  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  HRESULT hr = EnumDevices(&devices, &count);
  if (hr == E_NOTIMPL && !GetApi().missing.empty()) {
    *status = "no Media Foundation (missing " + GetApi().missing + ")";
  } else if (FAILED(hr)) {
    *status = "MFEnumDeviceSources failed: " + Hr(hr);
  } else {
    *status = "Media Foundation: " + std::to_string(count) + " video capture device" +
              (count == 1 ? "" : "s");
    for (UINT32 i = 0; i < count; ++i) {
      Device d;
      WCHAR* value = nullptr;
      UINT32 length = 0;
      if (SUCCEEDED(devices[i]->GetAllocatedString(kDevSourceSymbolicLink, &value, &length))) {
        d.id = Utf8(value);
        CoTaskMemFree(value);
      }
      value = nullptr;
      if (SUCCEEDED(devices[i]->GetAllocatedString(kDevSourceFriendlyName, &value, &length))) {
        d.name = Utf8(value);
        CoTaskMemFree(value);
      }
      IMFMediaSource* source = nullptr;
      IMFSourceReader* reader = nullptr;
      if (SUCCEEDED(NewReader(devices[i], &source, &reader))) {
        d.sizes = NativeSizes(reader);
        SafeRelease(reader);
        source->Shutdown();
        SafeRelease(source);
      }
      devices[i]->ShutdownObject();
      if (d.id.empty() || d.sizes.empty()) continue;
      *status += ", " + d.name;
      list.push_back(d);
    }
    FreeDevices(devices, count);
  }
  if (TestCameraWanted()) {
    Device t;
    t.id = kTestId;
    t.name = "Gesher Test Camera";
    t.test = true;
    IMFMediaSource* source = nullptr;
    IMFSourceReader* reader = nullptr;
    HRESULT thr = NewTestReader(&source, &reader);
    if (SUCCEEDED(thr)) {
      // Its sizes as the source reader reads them from the media source.
      t.sizes = NativeSizes(reader);
      SafeRelease(reader);
      source->Shutdown();
      SafeRelease(source);
      *status += "; test camera (GESHER_CAMERA_TEST) as a Media Foundation media source, " +
                 std::to_string(t.sizes.size()) + " YUY2 sizes";
    } else {
      t.sizes = {{640, 480}, {320, 240}};
      *status += "; test camera (GESHER_CAMERA_TEST) without Media Foundation (" + Hr(thr) +
                 "): frames made below it";
    }
    list.push_back(t);
  }
  return list;
}

// --- capture ---------------------------------------------------------------------------

// Waits for the next of 30 frames a second.
void Pace(ULONGLONG* next) {
  ULONGLONG now = GetTickCount64();
  if (*next != 0 && now < *next) Sleep(static_cast<DWORD>(*next - now));
  *next = std::max<ULONGLONG>(now, *next) + 33;
}

class MfCapture : public Capture {
 public:
  ~MfCapture() override {
    read_trace_.Log();
    SafeRelease(reader_);
    if (source_ != nullptr) source_->Shutdown();
    SafeRelease(source_);
  }

  HRESULT Init(IMFActivate* device, int w, int h) {
    HRESULT hr = NewReader(device, &source_, &reader_);
    if (FAILED(hr)) return hr;
    return Configure(w, h);
  }

  // The test camera's media source (TestSource).
  HRESULT InitTest(int w, int h) {
    HRESULT hr = NewTestReader(&source_, &reader_);
    if (FAILED(hr)) return hr;
    paced_ = true;
    return Configure(w, h);
  }

 private:
  HRESULT Configure(int w, int h) {
    HRESULT hr = S_OK;
    // The native size nearest to what was asked; NV12 out.
    std::vector<std::pair<int, int>> sizes = NativeSizes(reader_);
    if (sizes.empty()) return static_cast<HRESULT>(0xC00D36B4);  // MF_E_INVALIDMEDIATYPE
    std::pair<int, int> best = sizes[0];
    long bestDiff = -1;
    for (const auto& s : sizes) {
      long diff = labs(static_cast<long>(s.first) * s.second - static_cast<long>(w) * h);
      if (bestDiff < 0 || diff < bestDiff) {
        bestDiff = diff;
        best = s;
      }
    }
    IMFMediaType* type = nullptr;
    hr = GetApi().MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(kMtMajorType, kMediaTypeVideo);
    if (SUCCEEDED(hr)) hr = type->SetGUID(kMtSubtype, kVideoFormatNv12);
    if (SUCCEEDED(hr)) {
      hr = type->SetUINT64(kMtFrameSize, (static_cast<UINT64>(best.first) << 32) | best.second);
    }
    if (SUCCEEDED(hr)) hr = reader_->SetCurrentMediaType(kFirstVideoStream, nullptr, type);
    SafeRelease(type);
    if (FAILED(hr)) return hr;
    width = best.first;
    height = best.second;
    return S_OK;
  }

 public:
  HRESULT Read(uint8_t* nv12) override {
    TraceSample trace(read_trace_);
    if (paced_) Pace(&next_);
    trace.Step(0);
    for (;;) {
      DWORD stream = 0, flags = 0;
      LONGLONG timestamp = 0;
      IMFSample* sample = nullptr;
      HRESULT hr = reader_->ReadSample(kFirstVideoStream, 0, &stream, &flags, &timestamp, &sample);
      trace.Step(1);
      if (FAILED(hr)) return hr;
      if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) {
        SafeRelease(sample);
        return static_cast<HRESULT>(0xC00D3E84);  // MF_E_END_OF_STREAM
      }
      if (sample == nullptr) continue;  // a stream tick, no frame
      IMFMediaBuffer* buffer = nullptr;
      hr = sample->ConvertToContiguousBuffer(&buffer);
      SafeRelease(sample);
      trace.Step(2);
      if (FAILED(hr)) return hr;
      hr = CopyFrame(buffer, nv12);
      SafeRelease(buffer);
      trace.Step(3);
      return hr;
    }
  }

 private:
  HRESULT CopyFrame(IMFMediaBuffer* buffer, uint8_t* nv12) {
    size_t rows = static_cast<size_t>(height) * 3 / 2;
    IMF2DBuffer* buffer2d = nullptr;
    if (SUCCEEDED(buffer->QueryInterface(__uuidof(IMF2DBuffer),
                                         reinterpret_cast<void**>(&buffer2d)))) {
      BYTE* scan0 = nullptr;
      LONG pitch = 0;
      HRESULT hr = buffer2d->Lock2D(&scan0, &pitch);
      if (SUCCEEDED(hr)) {
        for (size_t y = 0; y < rows; ++y) {
          memcpy(nv12 + y * width, scan0 + static_cast<ptrdiff_t>(y) * pitch, width);
        }
        buffer2d->Unlock2D();
      }
      buffer2d->Release();
      return hr;
    }
    BYTE* data = nullptr;
    DWORD max = 0, length = 0;
    HRESULT hr = buffer->Lock(&data, &max, &length);
    if (FAILED(hr)) return hr;
    size_t need = static_cast<size_t>(width) * rows;
    memcpy(nv12, data, std::min<size_t>(need, length));
    buffer->Unlock();
    return length >= need ? S_OK : E_UNEXPECTED;
  }

  IMFMediaSource* source_ = nullptr;
  IMFSourceReader* reader_ = nullptr;
  bool paced_ = false;  // the test camera's source makes frames at once: 30 a second here
  ULONGLONG next_ = 0;
  TraceStats read_trace_{"MF read", {"pace", "ReadSample", "contiguous buffer", "copy frame"}};
};

// The test camera where Media Foundation is missing: the same picture, made
// here, 30 frames a second.
class TestCapture : public Capture {
 public:
  HRESULT Read(uint8_t* nv12) override {
    Pace(&next_);
    TestPictureNv12(nv12, width, height, frame_++);
    return S_OK;
  }

 private:
  ULONGLONG next_ = 0;
  int frame_ = 0;
};

}  // namespace

const std::vector<Device>& List(std::string* status) {
  static std::string found;
  static const std::vector<Device>* list = new std::vector<Device>(FindDevices(&found));
  if (status != nullptr) *status = found;
  return *list;
}

std::unique_ptr<Capture> Open(const std::string& id, int width, int height, HRESULT* hr) {
  if (id == kTestId && TestCameraWanted()) {
    if (GetApi().missing.empty()) {
      // Through Media Foundation: its source reader over TestSource.
      auto capture = std::make_unique<MfCapture>();
      *hr = capture->InitTest(width, height);
      if (FAILED(*hr)) return nullptr;
      capture->via = "Media Foundation (IMFSourceReader over the test media source, YUY2 to NV12)";
      return capture;
    }
    auto capture = std::make_unique<TestCapture>();
    // The test camera's sizes: 640x480 and 320x240.
    bool small = width * height <= 320 * 240;
    capture->width = small ? 320 : 640;
    capture->height = small ? 240 : 480;
    capture->via = "frames made without Media Foundation";
    *hr = S_OK;
    return capture;
  }
  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  *hr = EnumDevices(&devices, &count);
  if (FAILED(*hr)) return nullptr;
  std::unique_ptr<Capture> result;
  *hr = HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
  for (UINT32 i = 0; i < count && result == nullptr; ++i) {
    WCHAR* value = nullptr;
    UINT32 length = 0;
    if (FAILED(devices[i]->GetAllocatedString(kDevSourceSymbolicLink, &value, &length))) continue;
    bool match = Utf8(value) == id;
    CoTaskMemFree(value);
    if (!match) continue;
    auto capture = std::make_unique<MfCapture>();
    *hr = capture->Init(devices[i], width, height);
    capture->via = "Media Foundation (IMFSourceReader over the device)";
    if (SUCCEEDED(*hr)) result = std::move(capture);
  }
  FreeDevices(devices, count);
  return result;
}

}  // namespace gesher::devices::camera
