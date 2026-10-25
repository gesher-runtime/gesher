// Gesher media: WASAPI shared mode output (sink.h).
//
// One render stream per Android stream volume group in use, each in its own
// audio session of this process ("<app> (Media)", "<app> (Alarm)", ...), so
// the Windows volume mixer shows the app's streams the way Android's volume
// panel does. A session's volume is its stream's Android volume (index / max)
// and its mute; a change made in the volume mixer goes back to the shared
// state (shared_state.h), where AudioManager reads it. The streams render the
// mix format (float, the endpoint's rate and channels); a group with nothing
// playing for a second is stopped.

// mmreg.h needs what NOGDI (the build's default) leaves out.
#undef NOGDI
#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propidl.h>
#include <propsys.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "engine.h"
#include "log.h"
#include "shared_state.h"
#include "sink.h"

namespace gesher::media {
namespace {

// The GUIDs, spelled out (not all are in every libuuid).
const CLSID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C,
                                        {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIidIMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35,
                                     {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIidIAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32,
                              {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const IID kIidIAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483,
                                    {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
const IID kIidISimpleAudioVolume = {0x87CE5498, 0x68D6, 0x44E5,
                                    {0x92, 0x15, 0x6D, 0xA4, 0x7E, 0xF8, 0x83, 0xD8}};
const IID kIidIAudioSessionControl = {0xF4B1A599, 0x7266, 0x4319,
                                      {0xA8, 0xCA, 0xE7, 0x0A, 0xCB, 0x11, 0xE8, 0xCD}};
const IID kIidIAudioSessionEvents = {0x24918ACC, 0x64B3, 0x37C1,
                                     {0x8C, 0xA9, 0x74, 0xA6, 0x6E, 0x99, 0x57, 0xA8}};
const IID kIidIMMNotificationClient = {0x7991EEC9, 0x7E89, 0x4D85,
                                       {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010,
                            {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
// PKEY_Device_FriendlyName and PKEY_AudioEndpoint_FormFactor.
const PROPERTYKEY kKeyFriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD,
                                       {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};
const PROPERTYKEY kKeyFormFactor = {{0x1DA5D803, 0xD492, 0x4EDD,
                                     {0x8C, 0x23, 0xE0, 0xC0, 0xFF, 0xEE, 0x7F, 0x0E}}, 0};
// Gesher's own volume changes carry this context; the mixer's do not.
const GUID kGesherContext = {0x6765736B, 0x6865, 0x7200,
                             {0x61, 0x75, 0x64, 0x69, 0x6F, 0x00, 0x00, 0x01}};
// A session per group: {67657368-6572-0000-6175-64696F0000gg}.
GUID SessionGuid(int group) {
  GUID g = {0x67657368, 0x6572, 0x0000, {0x61, 0x75, 0x64, 0x69, 0x6F, 0x00, 0x00, 0x00}};
  g.Data4[7] = static_cast<unsigned char>(group);
  return g;
}

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

// Volume mixer changes to a group's session.
class SessionEvents : public IAudioSessionEvents {
 public:
  explicit SessionEvents(int stream) : stream_(stream) {}
  virtual ~SessionEvents() = default;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kIidIAudioSessionEvents)) {
      *out = static_cast<IAudioSessionEvents*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
  }

  HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float volume, BOOL mute,
                                                  LPCGUID context) override {
    if (context != nullptr && IsEqualGUID(*context, kGesherContext)) return S_OK;
    MEDIA_LOGI("audio: the volume mixer set %s to %.2f%s", GroupName(GroupOfStream(stream_)),
               volume, mute ? " (muted)" : "");
    SetStreamGainFromSession(stream_, volume, mute != FALSE);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float*, DWORD, LPCGUID) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason) override {
    return S_OK;
  }

 private:
  std::atomic<ULONG> refs_{1};
  int stream_;
};

// The default device changing (or going away).
class DeviceEvents : public IMMNotificationClient {
 public:
  virtual ~DeviceEvents() = default;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kIidIMMNotificationClient)) {
      *out = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (flow == eRender && role == eConsole) changed = true;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
    return S_OK;
  }

  std::atomic<bool> changed{false};

 private:
  std::atomic<ULONG> refs_{1};
};

class WasapiSink : public Sink {
 public:
  ~WasapiSink() override {
    for (auto& g : groups_) CloseGroup(g);
    if (enumerator_ != nullptr && device_events_ != nullptr) {
      enumerator_->UnregisterEndpointNotificationCallback(device_events_);
    }
    SafeRelease(device_events_);
    SafeRelease(device_);
    SafeRelease(enumerator_);
    if (mix_ != nullptr) CoTaskMemFree(mix_);
  }

  bool Open(const std::u16string& app_name, std::string* error) {
    app_name_ = app_name;
    HRESULT hr = CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  kIidIMMDeviceEnumerator, reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr)) {
      *error = "WASAPI: no device enumerator (" + Hr(hr) + ")";
      return false;
    }
    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr)) {
      *error = hr == static_cast<HRESULT>(0x80070490)
                   ? "WASAPI: no audio endpoint (" + Hr(hr) + ")"
                   : "WASAPI: no default render endpoint (" + Hr(hr) + ")";
      return false;
    }
    ReadDeviceProperties();
    IAudioClient* probe = nullptr;
    hr = device_->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&probe));
    if (FAILED(hr)) {
      *error = "WASAPI: IAudioClient (" + Hr(hr) + ")";
      return false;
    }
    hr = probe->GetMixFormat(&mix_);
    REFERENCE_TIME period = 0;
    probe->GetDevicePeriod(&period, nullptr);
    probe->Release();
    if (FAILED(hr) || mix_ == nullptr) {
      *error = "WASAPI: GetMixFormat (" + Hr(hr) + ")";
      return false;
    }
    rate_ = mix_->nSamplesPerSec;
    channels_ = std::clamp<uint32_t>(mix_->nChannels, 1, 8);
    bool is_float = mix_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                    (mix_->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                     IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix_)->SubFormat,
                                 kSubtypeFloat));
    if (!is_float || mix_->wBitsPerSample != 32) {
      // Ask for float stereo at the mix rate; the audio engine converts.
      convert_ = true;
      channels_ = 2;
    }
    memset(&format_, 0, sizeof(format_));
    format_.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format_.Format.nChannels = static_cast<WORD>(channels_);
    format_.Format.nSamplesPerSec = rate_;
    format_.Format.wBitsPerSample = 32;
    format_.Format.nBlockAlign = static_cast<WORD>(4 * channels_);
    format_.Format.nAvgBytesPerSec = rate_ * format_.Format.nBlockAlign;
    format_.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format_.Samples.wValidBitsPerSample = 32;
    format_.dwChannelMask = mix_->wFormatTag == WAVE_FORMAT_EXTENSIBLE && !convert_
                                ? reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix_)->dwChannelMask
                                : (channels_ == 1 ? SPEAKER_FRONT_CENTER
                                                  : SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    format_.SubFormat = kSubtypeFloat;
    device_period_frames_ = static_cast<uint32_t>(period * rate_ / 10000000);
    // Keep about 40 ms queued: the engine ticks every 10 ms.
    target_frames_ = std::max<uint32_t>(rate_ / 25, 2 * device_period_frames_);

    // Open the media group now: a missing or broken endpoint shows here, not
    // at the first sound.
    if (!OpenGroup(groups_[0], 0, error)) return false;
    device_events_ = new DeviceEvents();
    enumerator_->RegisterEndpointNotificationCallback(device_events_);
    return true;
  }

  uint32_t rate() const override { return rate_; }
  uint32_t channels() const override { return channels_; }
  uint32_t latency_frames() const override { return target_frames_ + latency_extra_frames_; }

  std::string DeviceName() const override { return device_name_; }
  int DeviceType() const override { return device_type_; }

  std::string Describe() const override {
    std::string s = "WASAPI shared mode on " + device_name_ + ", " + std::to_string(rate_) + " Hz, " +
                    std::to_string(channels_) + " channels, float";
    if (convert_) s += " (converted by Windows)";
    s += ", " + std::to_string(target_frames_ * 1000 / rate_) + " ms buffered";
    return s;
  }

  bool Tick(Mixer* mixer) override {
    if (device_events_ != nullptr && device_events_->changed.exchange(false)) {
      MEDIA_LOGI("audio: the default output device changed");
      return false;
    }
    for (int i = 0; i < kNumGroups; i++) {
      Group& g = groups_[i];
      bool wanted = mixer->GroupWanted(i);
      if (g.client == nullptr) {
        if (!wanted) continue;
        std::string error;
        if (!OpenGroup(g, i, &error)) {
          MEDIA_LOGE("audio: %s", error.c_str());
          return false;
        }
      }
      if (!wanted) {
        if (g.running) {
          g.client->Stop();
          g.client->Reset();
          g.running = false;
        }
        continue;
      }
      UINT32 padding = 0;
      HRESULT hr = g.client->GetCurrentPadding(&padding);
      if (FAILED(hr)) return Lost(hr);
      uint32_t target = std::min(target_frames_, g.buffer_frames);
      if (padding < target) {
        uint32_t n = target - padding;
        BYTE* data = nullptr;
        hr = g.render->GetBuffer(n, &data);
        if (FAILED(hr)) return Lost(hr);
        mixer->MixGroup(i, reinterpret_cast<float*>(data), n);
        hr = g.render->ReleaseBuffer(n, 0);
        if (FAILED(hr)) return Lost(hr);
      }
      if (!g.running) {
        hr = g.client->Start();
        if (FAILED(hr)) return Lost(hr);
        g.running = true;
      }
    }
    return true;
  }

  void OnVolumesChanged() override {
    for (int i = 0; i < kNumGroups; i++) ApplyVolume(groups_[i], i);
  }

 private:
  struct Group {
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    ISimpleAudioVolume* volume = nullptr;
    IAudioSessionControl* control = nullptr;
    SessionEvents* events = nullptr;
    UINT32 buffer_frames = 0;
    bool running = false;
  };

  bool Lost(HRESULT hr) {
    MEDIA_LOGW("audio: the output stopped (%s)", Hr(hr).c_str());
    return false;
  }

  bool OpenGroup(Group& g, int index, std::string* error) {
    HRESULT hr = device_->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&g.client));
    if (FAILED(hr)) {
      *error = "WASAPI: IAudioClient (" + Hr(hr) + ")";
      return false;
    }
    DWORD flags = AUDCLNT_STREAMFLAGS_NOPERSIST | AUDCLNT_SESSIONFLAGS_DISPLAY_HIDEWHENEXPIRED;
    if (convert_) flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    GUID session = SessionGuid(index);
    hr = g.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000 /* 100 ms */, 0,
                              &format_.Format, &session);
    if (FAILED(hr)) {
      *error = "WASAPI: Initialize (" + Hr(hr) + ")";
      CloseGroup(g);
      return false;
    }
    g.client->GetBufferSize(&g.buffer_frames);
    REFERENCE_TIME latency = 0;
    if (SUCCEEDED(g.client->GetStreamLatency(&latency))) {
      latency_extra_frames_ = static_cast<uint32_t>(latency * rate_ / 10000000);
    }
    hr = g.client->GetService(kIidIAudioRenderClient, reinterpret_cast<void**>(&g.render));
    if (FAILED(hr)) {
      *error = "WASAPI: IAudioRenderClient (" + Hr(hr) + ")";
      CloseGroup(g);
      return false;
    }
    g.client->GetService(kIidISimpleAudioVolume, reinterpret_cast<void**>(&g.volume));
    if (SUCCEEDED(g.client->GetService(kIidIAudioSessionControl,
                                       reinterpret_cast<void**>(&g.control)))) {
      std::u16string name = app_name_.empty() ? u"Android app" : app_name_;
      name += u" (";
      for (const char* p = GroupName(index); *p != 0; p++) name += static_cast<char16_t>(*p);
      name += u")";
      g.control->SetDisplayName(reinterpret_cast<LPCWSTR>(name.c_str()), nullptr);
      g.events = new SessionEvents(StreamOfGroup(index));
      g.control->RegisterAudioSessionNotification(g.events);
    }
    ApplyVolume(g, index);
    return true;
  }

  void ReadDeviceProperties() {
    IPropertyStore* props = nullptr;
    if (FAILED(device_->OpenPropertyStore(STGM_READ, &props))) return;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(props->GetValue(kKeyFriendlyName, &v)) && v.vt == VT_LPWSTR) {
      int n = WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, nullptr, 0, nullptr, nullptr);
      if (n > 1) {
        device_name_.assign(n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, device_name_.data(), n, nullptr, nullptr);
      }
    }
    PropVariantClear(&v);
    if (SUCCEEDED(props->GetValue(kKeyFormFactor, &v)) && v.vt == VT_UI4) {
      // EndpointFormFactor to AUDIO_DEVICE_OUT_*.
      switch (v.ulVal) {
        case 2: device_type_ = 0x20000; break;  // LineLevel: LINE
        case 3: device_type_ = 0x8; break;      // Headphones: WIRED_HEADPHONE
        case 5: device_type_ = 0x4; break;      // Headset: WIRED_HEADSET
        case 8: device_type_ = 0x80000; break;  // SPDIF
        case 9: device_type_ = 0x400; break;    // DigitalAudioDisplayDevice: HDMI
        default: device_type_ = 0x2; break;     // SPEAKER
      }
    }
    PropVariantClear(&v);
    props->Release();
  }

  void ApplyVolume(Group& g, int index) {
    if (g.volume == nullptr) return;
    int stream = StreamOfGroup(index);
    int max = GetStreamMaxVolume(stream);
    float level = max > 0 ? static_cast<float>(GetStreamVolume(stream)) / static_cast<float>(max) : 1.0f;
    g.volume->SetMasterVolume(std::clamp(level, 0.0f, 1.0f), &kGesherContext);
    g.volume->SetMute(GetStreamMuted(stream) ? TRUE : FALSE, &kGesherContext);
  }

  void CloseGroup(Group& g) {
    if (g.client != nullptr && g.running) g.client->Stop();
    if (g.control != nullptr && g.events != nullptr) {
      g.control->UnregisterAudioSessionNotification(g.events);
    }
    SafeRelease(g.events);
    SafeRelease(g.control);
    SafeRelease(g.volume);
    SafeRelease(g.render);
    SafeRelease(g.client);
    g.running = false;
  }

  std::u16string app_name_;
  std::string device_name_ = "Windows default output";
  int device_type_ = 0x2;
  IMMDeviceEnumerator* enumerator_ = nullptr;
  IMMDevice* device_ = nullptr;
  DeviceEvents* device_events_ = nullptr;
  WAVEFORMATEX* mix_ = nullptr;
  WAVEFORMATEXTENSIBLE format_;
  bool convert_ = false;
  uint32_t rate_ = 48000;
  uint32_t channels_ = 2;
  uint32_t device_period_frames_ = 0;
  uint32_t target_frames_ = 1920;
  uint32_t latency_extra_frames_ = 0;
  Group groups_[kNumGroups];
};

}  // namespace

std::unique_ptr<Sink> OpenWasapiSink(const std::u16string& app_name, std::string* error) {
  auto sink = std::make_unique<WasapiSink>();
  if (!sink->Open(app_name, error)) return nullptr;
  return sink;
}

}  // namespace gesher::media
