// Gesher media: WASAPI shared mode capture (capture.h; ADR 0059): the default
// capture endpoint (the PC's microphone), or the default render endpoint's
// loopback. The mix format (float, the endpoint's rate and channels), polled
// by the capture thread every 10 ms.

// mmreg.h needs what NOGDI (the build's default) leaves out.
#undef NOGDI
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propidl.h>
#include <propsys.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "capture.h"
#include "log.h"

namespace gesher::media {
namespace {

const CLSID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C,
                                        {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIidIMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35,
                                     {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIidIAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32,
                              {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const IID kIidIAudioCaptureClient = {0xC8ADBD64, 0xE71E, 0x48A0,
                                     {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010,
                            {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010,
                          {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const PROPERTYKEY kKeyFriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD,
                                       {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

template <typename T>
void SafeRelease(T*& p) {
  if (p != nullptr) {
    p->Release();
    p = nullptr;
  }
}

std::string Hr(HRESULT hr) {
  char buf[32];
  snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(hr));
  return buf;
}

std::string Narrow(const wchar_t* w) {
  if (w == nullptr) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::string FriendlyName(IMMDevice* device) {
  IPropertyStore* props = nullptr;
  std::string name;
  if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(props->GetValue(kKeyFriendlyName, &v)) && v.vt == VT_LPWSTR) name = Narrow(v.pwszVal);
    PropVariantClear(&v);
    props->Release();
  }
  return name;
}

// The default endpoint of `flow` (eCapture, or eRender for loopback).
HRESULT DefaultEndpoint(EDataFlow flow, IMMDevice** device) {
  IMMDeviceEnumerator* e = nullptr;
  HRESULT hr = CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIidIMMDeviceEnumerator,
                                reinterpret_cast<void**>(&e));
  if (FAILED(hr)) return hr;
  hr = e->GetDefaultAudioEndpoint(flow, eConsole, device);
  e->Release();
  return hr;
}

class WasapiCapture : public CaptureSource {
 public:
  ~WasapiCapture() override {
    if (client_ != nullptr) client_->Stop();
    SafeRelease(capture_);
    SafeRelease(client_);
    SafeRelease(device_);
    if (format_ != nullptr) CoTaskMemFree(format_);
  }

  bool Open(bool loopback, std::string* error) {
    loopback_ = loopback;
    HRESULT hr = DefaultEndpoint(loopback ? eRender : eCapture, &device_);
    if (FAILED(hr) || device_ == nullptr) {
      *error = std::string("WASAPI: no audio ") + (loopback ? "output for loopback" : "capture endpoint") +
               " (" + Hr(hr) + ")";
      return false;
    }
    name_ = FriendlyName(device_);
    hr = device_->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
    if (SUCCEEDED(hr)) hr = client_->GetMixFormat(&format_);
    if (FAILED(hr)) {
      *error = "WASAPI capture: " + Hr(hr);
      return false;
    }
    channels_ = format_->nChannels;
    rate_ = format_->nSamplesPerSec;
    bits_ = format_->wBitsPerSample;
    is_float_ = format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
      auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format_);
      is_float_ = IsEqualGUID(ext->SubFormat, kSubtypeFloat);
      if (!is_float_ && !IsEqualGUID(ext->SubFormat, kSubtypePcm)) {
        *error = "WASAPI capture: an unknown mix format";
        return false;
      }
    }
    // 200 ms buffered, shared mode, polled.
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
                             2000000, 0, format_, nullptr);
    if (SUCCEEDED(hr)) {
      hr = client_->GetService(kIidIAudioCaptureClient, reinterpret_cast<void**>(&capture_));
    }
    if (FAILED(hr)) {
      *error = "WASAPI capture: Initialize " + Hr(hr);
      return false;
    }
    return true;
  }

  uint32_t rate() const override { return rate_; }
  uint32_t channels() const override { return channels_; }
  std::string Describe() const override {
    return std::string("WASAPI shared mode ") + (loopback_ ? "loopback of " : "capture on ") + name_ + ", " +
           std::to_string(rate_) + " Hz, " + std::to_string(channels_) + " channels" +
           (is_float_ ? ", float" : "");
  }
  std::string DeviceName() const override { return name_; }
  bool Start() override { return SUCCEEDED(client_->Start()); }
  void Stop() override { client_->Stop(); }

  bool Pull(std::vector<float>* out) override {
    for (int guard = 0; guard < 64; guard++) {
      UINT32 packet = 0;
      HRESULT hr = capture_->GetNextPacketSize(&packet);
      if (FAILED(hr)) return hr != AUDCLNT_E_DEVICE_INVALIDATED ? true : false;
      if (packet == 0) return true;
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
      if (FAILED(hr)) return hr != AUDCLNT_E_DEVICE_INVALIDATED;
      const size_t at = out->size();
      out->resize(at + static_cast<size_t>(frames) * channels_);
      float* d = out->data() + at;
      const size_t count = static_cast<size_t>(frames) * channels_;
      if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
        std::fill(d, d + count, 0.0f);
      } else if (is_float_ && bits_ == 32) {
        memcpy(d, data, count * sizeof(float));
      } else if (bits_ == 16) {
        const int16_t* s = reinterpret_cast<const int16_t*>(data);
        for (size_t i = 0; i < count; i++) d[i] = s[i] / 32768.0f;
      } else if (bits_ == 32) {
        const int32_t* s = reinterpret_cast<const int32_t*>(data);
        for (size_t i = 0; i < count; i++) d[i] = static_cast<float>(s[i] / 2147483648.0);
      } else {
        std::fill(d, d + count, 0.0f);
      }
      capture_->ReleaseBuffer(frames);
    }
    return true;
  }

 private:
  bool loopback_ = false;
  IMMDevice* device_ = nullptr;
  IAudioClient* client_ = nullptr;
  IAudioCaptureClient* capture_ = nullptr;
  WAVEFORMATEX* format_ = nullptr;
  uint32_t rate_ = 0, channels_ = 0, bits_ = 0;
  bool is_float_ = false;
  std::string name_;
};

}  // namespace

void CaptureComInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }

std::unique_ptr<CaptureSource> OpenWasapiCapture(bool loopback, std::string* error) {
  auto c = std::make_unique<WasapiCapture>();
  if (!c->Open(loopback, error)) return nullptr;
  return c;
}

bool HasWasapiCaptureEndpoint(std::string* name) {
  // On a thread of its own: the caller's apartment is not known.
  bool found = false;
  std::string n;
  std::pair<bool*, std::string*> result(&found, &n);
  HANDLE t = CreateThread(
      nullptr, 0,
      [](void* arg) -> DWORD {
        auto* r = static_cast<std::pair<bool*, std::string*>*>(arg);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDevice* device = nullptr;
        if (SUCCEEDED(DefaultEndpoint(eCapture, &device)) && device != nullptr) {
          *r->first = true;
          *r->second = FriendlyName(device);
          device->Release();
        }
        CoUninitialize();
        return 0;
      },
      &result, 0, nullptr);
  if (t != nullptr) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
  }
  if (name != nullptr) *name = n;
  return found;
}

}  // namespace gesher::media
