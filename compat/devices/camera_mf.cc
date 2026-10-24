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
constexpr GUID kReaderVideoProcessing = {0xfb394f3d, 0xccf1, 0x42ee,
                                         {0xbb, 0xb3, 0xf9, 0xb8, 0x45, 0xd5, 0x68, 0x1d}};
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

// A source reader over a device's media source, with video processing (NV12 out).
HRESULT NewReader(IMFActivate* device, IMFMediaSource** source, IMFSourceReader** reader) {
  const Api& api = GetApi();
  *source = nullptr;
  *reader = nullptr;
  HRESULT hr = device->ActivateObject(__uuidof(IMFMediaSource), reinterpret_cast<void**>(source));
  if (FAILED(hr)) return hr;
  IMFAttributes* attributes = nullptr;
  hr = api.MFCreateAttributes(&attributes, 1);
  if (SUCCEEDED(hr)) hr = attributes->SetUINT32(kReaderVideoProcessing, TRUE);
  if (SUCCEEDED(hr)) hr = api.MFCreateSourceReaderFromMediaSource(*source, attributes, reader);
  SafeRelease(attributes);
  if (FAILED(hr)) {
    (*source)->Shutdown();
    SafeRelease(*source);
  }
  return hr;
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
    t.sizes = {{640, 480}, {320, 240}};
    list.push_back(t);
    *status += "; test camera (GESHER_CAMERA_TEST)";
  }
  return list;
}

// --- capture ---------------------------------------------------------------------------

class MfCapture : public Capture {
 public:
  ~MfCapture() override {
    SafeRelease(reader_);
    if (source_ != nullptr) source_->Shutdown();
    SafeRelease(source_);
  }

  HRESULT Init(IMFActivate* device, int w, int h) {
    HRESULT hr = NewReader(device, &source_, &reader_);
    if (FAILED(hr)) return hr;
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

  HRESULT Read(uint8_t* nv12) override {
    for (;;) {
      DWORD stream = 0, flags = 0;
      LONGLONG timestamp = 0;
      IMFSample* sample = nullptr;
      HRESULT hr = reader_->ReadSample(kFirstVideoStream, 0, &stream, &flags, &timestamp, &sample);
      if (FAILED(hr)) return hr;
      if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) {
        SafeRelease(sample);
        return static_cast<HRESULT>(0xC00D3E84);  // MF_E_END_OF_STREAM
      }
      if (sample == nullptr) continue;  // a stream tick, no frame
      IMFMediaBuffer* buffer = nullptr;
      hr = sample->ConvertToContiguousBuffer(&buffer);
      SafeRelease(sample);
      if (FAILED(hr)) return hr;
      hr = CopyFrame(buffer, nv12);
      SafeRelease(buffer);
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
};

// The test camera: 30 frames a second of a known picture (Y = x + y, gray
// chroma) with a bright 32-pixel square moving across row 0..31, so frames
// differ while row 100 stays known.
class TestCapture : public Capture {
 public:
  HRESULT Read(uint8_t* nv12) override {
    ULONGLONG now = GetTickCount64();
    if (next_ != 0 && now < next_) Sleep(static_cast<DWORD>(next_ - now));
    next_ = std::max<ULONGLONG>(now, next_) + 33;
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) nv12[y * width + x] = static_cast<uint8_t>(x + y);
    }
    int square = (frame_ * 8) % std::max(width - 32, 1);
    for (int y = 0; y < 32 && y < height; ++y) {
      memset(nv12 + y * width + square, 235, std::min(32, width - square));
    }
    memset(nv12 + static_cast<size_t>(width) * height, 128, static_cast<size_t>(width) * height / 2);
    frame_++;
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
    auto capture = std::make_unique<TestCapture>();
    // The test camera's sizes: 640x480 and 320x240.
    bool small = width * height <= 320 * 240;
    capture->width = small ? 320 : 640;
    capture->height = small ? 240 : 480;
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
    if (SUCCEEDED(*hr)) result = std::move(capture);
  }
  FreeDevices(devices, count);
  return result;
}

}  // namespace gesher::devices::camera
