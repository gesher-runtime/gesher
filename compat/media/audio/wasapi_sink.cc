// Gesher media: low-latency event-driven WASAPI audio sink implementation.

#undef NOGDI
#undef WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <avrt.h>
#include <propidl.h>
#include <propsys.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "log.h"
#include "shared_state.h"
#include "wasapi_sink.h"

namespace gesher::media {
namespace {

// Explicit COM interface IDs and Class IDs
const CLSID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C,
                                        {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIidIMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35,
                                     {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIidIAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32,
                              {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const IID kIidIAudioClient3 = {0x7ED4EE07, 0x8E67, 0x4CD4,
                               {0x8C, 0x1A, 0x2B, 0x7A, 0x56, 0x62, 0xB0, 0xAE}};
const IID kIidIAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483,
                                    {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
const IID kIidISimpleAudioVolume = {0x87CE5498, 0x68D6, 0x44E5,
                                    {0x92, 0x15, 0x6D, 0xA4, 0x7E, 0xF8, 0x83, 0xD8}};
const IID kIidIAudioSessionControl = {0xF4B1A599, 0x7266, 0x4319,
                                      {0xA8, 0xCA, 0xE7, 0x0A, 0xCB, 0x11, 0xE8, 0xCD}};

const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010,
                            {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010,
                          {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

const PROPERTYKEY kKeyFriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD,
                                       {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};
const PROPERTYKEY kKeyFormFactor = {{0x1DA5D803, 0xD492, 0x4EDD,
                                     {0x8C, 0x23, 0xE0, 0xC0, 0xFF, 0xEE, 0x7F, 0x0E}}, 0};

const GUID kGesherSessionContext = {0x6765736B, 0x6865, 0x7200,
                                    {0x61, 0x75, 0x64, 0x69, 0x6F, 0x00, 0x00, 0x01}};

GUID SessionGuidForGroup(int group) {
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

std::string HrToString(HRESULT hr) {
  char buf[32];
  snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(hr));
  return buf;
}

}  // namespace

#ifndef __IAudioClient3_INTERFACE_DEFINED__
#define __IAudioClient3_INTERFACE_DEFINED__
MIDL_INTERFACE("7ED4EE07-8E67-4CD4-8C1A-2B7A5662B0AE")
IAudioClient3 : public IAudioClient2 {
 public:
  virtual HRESULT STDMETHODCALLTYPE GetBufferSizeLimits(
      const WAVEFORMATEX *pFormat,
      BOOL bEventDriven,
      REFERENCE_TIME *phnsMinBufferDuration,
      REFERENCE_TIME *phnsMaxBufferDuration) = 0;

  virtual HRESULT STDMETHODCALLTYPE GetSharedModeEnginePeriod(
      const WAVEFORMATEX *pFormat,
      UINT32 *pDefaultPeriodInFrames,
      UINT32 *pFundamentalPeriodInFrames,
      UINT32 *pMinPeriodInFrames,
      UINT32 *pMaxPeriodInFrames) = 0;

  virtual HRESULT STDMETHODCALLTYPE GetCurrentSharedModeEnginePeriod(
      WAVEFORMATEX **ppFormat,
      UINT32 *pCurrentPeriodInFrames) = 0;

  virtual HRESULT STDMETHODCALLTYPE InitializeSharedAudioStream(
      DWORD StreamFlags,
      UINT32 PeriodInFrames,
      const WAVEFORMATEX *pFormat,
      LPCGUID AudioSessionGuid) = 0;
};
#endif

bool BuildWaveFormatEx(uint32_t rate, uint32_t channels, WasapiSampleFormat format,
                       WAVEFORMATEX* out_wf) {
  if (out_wf == nullptr) return false;
  if (channels == 0 || channels > 8) channels = 2;
  if (rate != 44100 && rate != 48000) rate = 48000;

  memset(out_wf, 0, sizeof(*out_wf));
  WORD bits = (format == WasapiSampleFormat::kPcm16) ? 16 : 32;
  WORD block_align = static_cast<WORD>(channels * (bits / 8));

  out_wf->wFormatTag = (format == WasapiSampleFormat::kPcm16) ? WAVE_FORMAT_PCM : WAVE_FORMAT_IEEE_FLOAT;
  out_wf->nChannels = static_cast<WORD>(channels);
  out_wf->nSamplesPerSec = rate;
  out_wf->nAvgBytesPerSec = rate * block_align;
  out_wf->nBlockAlign = block_align;
  out_wf->wBitsPerSample = bits;
  out_wf->cbSize = 0;
  return true;
}

bool BuildWaveFormatExtensible(uint32_t rate, uint32_t channels, WasapiSampleFormat format,
                               WAVEFORMATEXTENSIBLE* out_wfe) {
  if (out_wfe == nullptr) return false;
  if (channels == 0 || channels > 8) channels = 2;
  if (rate != 44100 && rate != 48000) rate = 48000;

  memset(out_wfe, 0, sizeof(*out_wfe));
  WORD bits = (format == WasapiSampleFormat::kPcm16) ? 16 : 32;
  WORD block_align = static_cast<WORD>(channels * (bits / 8));

  out_wfe->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  out_wfe->Format.nChannels = static_cast<WORD>(channels);
  out_wfe->Format.nSamplesPerSec = rate;
  out_wfe->Format.nAvgBytesPerSec = rate * block_align;
  out_wfe->Format.nBlockAlign = block_align;
  out_wfe->Format.wBitsPerSample = bits;
  out_wfe->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  out_wfe->Samples.wValidBitsPerSample = bits;

  if (channels == 1) {
    out_wfe->dwChannelMask = SPEAKER_FRONT_CENTER;
  } else {
    out_wfe->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  }

  out_wfe->SubFormat = (format == WasapiSampleFormat::kPcm16) ? kSubtypePcm : kSubtypeFloat;
  return true;
}

WasapiAudioSink::WasapiAudioSink() = default;

WasapiAudioSink::~WasapiAudioSink() {
  Close();
}

bool WasapiAudioSink::Initialize(const WasapiSinkParams& params, std::string* error) {
  params_ = params;
  app_name_ = params.app_name;
  rate_ = (params.sample_rate == 44100 || params.sample_rate == 48000) ? params.sample_rate : 48000;
  channels_ = (params.channels > 0 && params.channels <= 8) ? params.channels : 2;
  bytes_per_sample_ = (params.format == WasapiSampleFormat::kPcm16) ? 2 : 4;
  bytes_per_frame_ = channels_ * bytes_per_sample_;

  // Build format descriptors
  BuildWaveFormatExtensible(rate_, channels_, params.format, &format_ext_);
  BuildWaveFormatEx(rate_, channels_, params.format, &format_ex_);

  // Allocate thread-safe ring buffer (holds ~250ms of audio)
  size_t ring_capacity = rate_ * bytes_per_frame_ / 4;
  ring_buffer_.assign(ring_capacity, 0);
  ring_head_ = 0;
  ring_tail_ = 0;
  ring_count_ = 0;

  if (!InitAudioClient(error)) {
    Close();
    return false;
  }

  // Create event handles for event-driven buffering
  buffer_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (buffer_event_ == nullptr || stop_event_ == nullptr) {
    *error = "WASAPI: failed to create synchronization events";
    Close();
    return false;
  }

  HRESULT hr = client_->SetEventHandle(buffer_event_);
  if (FAILED(hr)) {
    *error = "WASAPI: SetEventHandle failed (" + HrToString(hr) + ")";
    Close();
    return false;
  }

  hr = client_->GetService(kIidIAudioRenderClient, reinterpret_cast<void**>(&render_));
  if (FAILED(hr) || render_ == nullptr) {
    *error = "WASAPI: IAudioRenderClient (" + HrToString(hr) + ")";
    Close();
    return false;
  }

  client_->GetService(kIidISimpleAudioVolume, reinterpret_cast<void**>(&simple_volume_));
  if (SUCCEEDED(client_->GetService(kIidIAudioSessionControl,
                                    reinterpret_cast<void**>(&session_control_)))) {
    std::u16string display_name = app_name_.empty() ? u"Android audio" : app_name_;
    session_control_->SetDisplayName(reinterpret_cast<LPCWSTR>(display_name.c_str()), nullptr);
  }

  ApplyVolume();

  // Start background feeder thread
  quit_ = false;
  feeder_thread_ = std::thread(&WasapiAudioSink::FeedLoop, this);

  MEDIA_LOGI("WASAPI: initialized %s (%s, %u Hz, %u ch, buffer %u frames, period %u frames)",
             using_client3_ ? "IAudioClient3 (low-latency)" : "IAudioClient (event-driven)",
             params_.format == WasapiSampleFormat::kPcm16 ? "PCM 16" : "Float 32",
             rate_, channels_, buffer_frames_, period_frames_);
  return true;
}

bool WasapiAudioSink::InitAudioClient(std::string* error) {
  HRESULT hr = CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                kIidIMMDeviceEnumerator, reinterpret_cast<void**>(&enumerator_));
  if (FAILED(hr) || enumerator_ == nullptr) {
    *error = "WASAPI: no device enumerator (" + HrToString(hr) + ")";
    return false;
  }

  hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
  if (FAILED(hr) || device_ == nullptr) {
    *error = hr == static_cast<HRESULT>(0x80070490)
                 ? "WASAPI: no audio endpoint (" + HrToString(hr) + ")"
                 : "WASAPI: no default render endpoint (" + HrToString(hr) + ")";
    return false;
  }

  ReadDeviceProperties();

  hr = device_->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
  if (FAILED(hr) || client_ == nullptr) {
    *error = "WASAPI: IAudioClient activation failed (" + HrToString(hr) + ")";
    return false;
  }

  GUID session = SessionGuidForGroup(params_.stream_group);
  DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;

  // Try IAudioClient3 for ultra low latency shared mode if requested
  IAudioClient3* client3 = nullptr;
  if (params_.low_latency && SUCCEEDED(client_->QueryInterface(kIidIAudioClient3, reinterpret_cast<void**>(&client3)))) {
    UINT32 default_period = 0, fundamental_period = 0, min_period = 0, max_period = 0;
    hr = client3->GetSharedModeEnginePeriod(&format_ext_.Format, &default_period, &fundamental_period,
                                            &min_period, &max_period);
    if (SUCCEEDED(hr) && min_period > 0) {
      hr = client3->InitializeSharedAudioStream(stream_flags, min_period, &format_ext_.Format, &session);
      if (SUCCEEDED(hr)) {
        using_client3_ = true;
        period_frames_ = min_period;
      }
    }
    client3->Release();
  }

  // Fallback to standard IAudioClient event-driven shared mode
  if (!using_client3_) {
    REFERENCE_TIME target_hns = static_cast<REFERENCE_TIME>(params_.target_latency_ms) * 10000;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, target_hns, 0,
                             &format_ext_.Format, &session);
    if (FAILED(hr)) {
      // Try with standard WAVEFORMATEX
      hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, target_hns, 0,
                               &format_ex_, &session);
      if (SUCCEEDED(hr)) use_extensible_ = false;
    }
    if (FAILED(hr)) {
      *error = "WASAPI: Initialize shared mode failed (" + HrToString(hr) + ")";
      return false;
    }
  }

  client_->GetBufferSize(&buffer_frames_);
  REFERENCE_TIME stream_latency = 0;
  if (SUCCEEDED(client_->GetStreamLatency(&stream_latency))) {
    latency_extra_frames_ = static_cast<uint32_t>(stream_latency * rate_ / 10000000);
  }
  if (period_frames_ == 0) {
    period_frames_ = buffer_frames_ / 2;
  }
  return true;
}

void WasapiAudioSink::Close() {
  quit_ = true;
  if (stop_event_ != nullptr) SetEvent(stop_event_);
  ring_cv_.notify_all();

  if (feeder_thread_.joinable()) feeder_thread_.join();

  if (client_ != nullptr && running_) client_->Stop();
  running_ = false;

  SafeRelease(session_control_);
  SafeRelease(simple_volume_);
  SafeRelease(render_);
  SafeRelease(client_);
  SafeRelease(device_);
  SafeRelease(enumerator_);

  if (buffer_event_ != nullptr) {
    CloseHandle(buffer_event_);
    buffer_event_ = nullptr;
  }
  if (stop_event_ != nullptr) {
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
  }
  ClearOpenSLQueue();
}

uint32_t WasapiAudioSink::latency_frames() const {
  return buffer_frames_ + latency_extra_frames_;
}

std::string WasapiAudioSink::Describe() const {
  std::string s = "WASAPI shared mode on " + device_name_ + ", " + std::to_string(rate_) + " Hz, " +
                  std::to_string(channels_) + " channels, " +
                  (params_.format == WasapiSampleFormat::kPcm16 ? "pcm16" : "float");
  if (using_client3_) {
    s += ", low-latency (IAudioClient3, period " + std::to_string(period_frames_) + " frames)";
  } else {
    s += ", event-driven (buffer " + std::to_string(buffer_frames_) + " frames)";
  }
  return s;
}

bool WasapiAudioSink::Start() {
  if (running_) return true;
  if (client_ == nullptr) return false;
  HRESULT hr = client_->Start();
  if (SUCCEEDED(hr)) {
    running_ = true;
    return true;
  }
  return false;
}

void WasapiAudioSink::Stop() {
  if (!running_) return;
  if (client_ != nullptr) client_->Stop();
  running_ = false;
}

void WasapiAudioSink::Pause() {
  Stop();
}

void WasapiAudioSink::Flush() {
  std::lock_guard<std::mutex> lock(ring_mu_);
  ring_head_ = 0;
  ring_tail_ = 0;
  ring_count_ = 0;
  ClearOpenSLQueue();
}

void WasapiAudioSink::Reset() {
  Stop();
  Flush();
  if (client_ != nullptr) client_->Reset();
}

size_t WasapiAudioSink::WriteFrames(const void* data, size_t frame_count, bool blocking) {
  if (data == nullptr || frame_count == 0) return 0;
  size_t bytes = frame_count * bytes_per_frame_;
  size_t written_bytes = WriteBytes(data, bytes, blocking);
  return written_bytes / bytes_per_frame_;
}

size_t WasapiAudioSink::WriteBytes(const void* data, size_t byte_count, bool blocking) {
  if (data == nullptr || byte_count == 0) return 0;
  const uint8_t* src = static_cast<const uint8_t*>(data);
  size_t total_written = 0;

  std::unique_lock<std::mutex> lock(ring_mu_);
  while (total_written < byte_count && !quit_) {
    size_t space = ring_buffer_.size() - ring_count_;
    if (space == 0) {
      if (!blocking) break;
      ring_cv_.wait(lock, [this] { return quit_ || (ring_buffer_.size() - ring_count_) > 0; });
      if (quit_) break;
      space = ring_buffer_.size() - ring_count_;
    }

    size_t to_write = std::min(byte_count - total_written, space);
    size_t first_chunk = std::min(to_write, ring_buffer_.size() - ring_tail_);
    memcpy(ring_buffer_.data() + ring_tail_, src + total_written, first_chunk);
    size_t second_chunk = to_write - first_chunk;
    if (second_chunk > 0) {
      memcpy(ring_buffer_.data(), src + total_written + first_chunk, second_chunk);
    }

    ring_tail_ = (ring_tail_ + to_write) % ring_buffer_.size();
    ring_count_ += to_write;
    total_written += to_write;
  }

  if (total_written > 0 && !running_) {
    Start();
  }
  return total_written;
}

ssize_t WasapiAudioSink::WriteAudioTrack(const void* data, size_t byte_count, bool is_float, bool blocking) {
  if (data == nullptr || byte_count == 0) return 0;

  // Direct format match
  bool native_float = (params_.format == WasapiSampleFormat::kFloat32);
  if (is_float == native_float) {
    return static_cast<ssize_t>(WriteBytes(data, byte_count, blocking));
  }

  // Format conversion
  if (is_float && !native_float) {
    // Float to PCM 16-bit
    size_t float_count = byte_count / sizeof(float);
    const float* in = static_cast<const float*>(data);
    std::vector<int16_t> pcm(float_count);
    for (size_t i = 0; i < float_count; i++) {
      float v = in[i];
      pcm[i] = static_cast<int16_t>(std::clamp(static_cast<int>(v * 32767.0f), -32768, 32767));
    }
    size_t written = WriteBytes(pcm.data(), pcm.size() * sizeof(int16_t), blocking);
    return static_cast<ssize_t>((written / sizeof(int16_t)) * sizeof(float));
  } else {
    // PCM 16-bit to Float
    size_t short_count = byte_count / sizeof(int16_t);
    const int16_t* in = static_cast<const int16_t*>(data);
    std::vector<float> flt(short_count);
    for (size_t i = 0; i < short_count; i++) {
      flt[i] = static_cast<float>(in[i]) / 32768.0f;
    }
    size_t written = WriteBytes(flt.data(), flt.size() * sizeof(float), blocking);
    return static_cast<ssize_t>((written / sizeof(float)) * sizeof(int16_t));
  }
}

void WasapiAudioSink::SetAudioTrackVolume(float left, float right) {
  volume_left_ = std::clamp(left, 0.0f, 1.0f);
  volume_right_ = std::clamp(right, 0.0f, 1.0f);
  if (simple_volume_ != nullptr) {
    float max_vol = std::max(volume_left_, volume_right_);
    simple_volume_->SetMasterVolume(max_vol, &kGesherSessionContext);
  }
}

bool WasapiAudioSink::EnqueueOpenSLBuffer(const void* buffer, size_t size_bytes,
                                         std::function<void(void*)> callback, void* context) {
  if (buffer == nullptr || size_bytes == 0) return false;
  OpenSLBufferItem item;
  item.data = buffer;
  item.size_bytes = size_bytes;
  item.consumed_bytes = 0;
  item.callback = std::move(callback);
  item.context = context;

  {
    std::lock_guard<std::mutex> lock(opensl_mu_);
    opensl_queue_.push_back(std::move(item));
  }
  if (!running_) Start();
  return true;
}

void WasapiAudioSink::ClearOpenSLQueue() {
  std::lock_guard<std::mutex> lock(opensl_mu_);
  opensl_queue_.clear();
}

size_t WasapiAudioSink::GetOpenSLQueueDepth() const {
  std::lock_guard<std::mutex> lock(opensl_mu_);
  return opensl_queue_.size();
}

bool WasapiAudioSink::Tick(Mixer* mixer) {
  if (mixer == nullptr) return false;
  if (!mixer->GroupWanted(params_.stream_group)) return true;

  // Mix a period's worth of frames from the mixer into our ring buffer
  uint32_t frames_to_mix = period_frames_ > 0 ? period_frames_ : (rate_ / 100);
  std::vector<float> mix_buf(frames_to_mix * channels_, 0.0f);
  if (mixer->MixGroup(params_.stream_group, mix_buf.data(), frames_to_mix)) {
    if (params_.format == WasapiSampleFormat::kFloat32) {
      WriteBytes(mix_buf.data(), mix_buf.size() * sizeof(float), false);
    } else {
      std::vector<int16_t> pcm(mix_buf.size());
      for (size_t i = 0; i < mix_buf.size(); i++) {
        pcm[i] = static_cast<int16_t>(std::clamp(static_cast<int>(mix_buf[i] * 32767.0f), -32768, 32767));
      }
      WriteBytes(pcm.data(), pcm.size() * sizeof(int16_t), false);
    }
  }
  return true;
}

void WasapiAudioSink::OnVolumesChanged() {
  ApplyVolume();
}

void WasapiAudioSink::ApplyVolume() {
  if (simple_volume_ == nullptr) return;
  int stream = StreamOfGroup(params_.stream_group);
  int max = GetStreamMaxVolume(stream);
  float level = max > 0 ? static_cast<float>(GetStreamVolume(stream)) / static_cast<float>(max) : 1.0f;
  simple_volume_->SetMasterVolume(std::clamp(level, 0.0f, 1.0f), &kGesherSessionContext);
  simple_volume_->SetMute(GetStreamMuted(stream) ? TRUE : FALSE, &kGesherSessionContext);
}

void WasapiAudioSink::ReadDeviceProperties() {
  IPropertyStore* props = nullptr;
  if (FAILED(device_->OpenPropertyStore(STGM_READ, &props)) || props == nullptr) return;

  PROPVARIANT v;
  PropVariantInit(&v);
  if (SUCCEEDED(props->GetValue(kKeyFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal != nullptr) {
    int n = WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
      device_name_.assign(n - 1, '\0');
      WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, device_name_.data(), n, nullptr, nullptr);
    }
  }
  PropVariantClear(&v);

  if (SUCCEEDED(props->GetValue(kKeyFormFactor, &v)) && v.vt == VT_UI4) {
    switch (v.ulVal) {
      case 2: device_type_ = 0x20000; break;  // Line
      case 3: device_type_ = 0x8; break;      // Headphones
      case 5: device_type_ = 0x4; break;      // Headset
      case 8: device_type_ = 0x80000; break;  // SPDIF
      case 9: device_type_ = 0x400; break;    // HDMI
      default: device_type_ = 0x2; break;     // Speaker
    }
  }
  PropVariantClear(&v);
  props->Release();
}

void WasapiAudioSink::FeedLoop() {
  // Join Pro Audio / Audio MMCSS task via avrt.dll
  DWORD task_index = 0;
  HANDLE avrt_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
  if (avrt_handle == nullptr) {
    avrt_handle = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
  }

  HANDLE wait_handles[2] = {stop_event_, buffer_event_};

  while (!quit_) {
    DWORD wait_res = WaitForMultipleObjects(2, wait_handles, FALSE, 2000);
    if (wait_res == WAIT_OBJECT_0) {
      // stop_event_ signaled
      break;
    }
    if (wait_res != WAIT_OBJECT_0 + 1) {
      // Timeout or error
      continue;
    }

    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (FAILED(hr)) break;

    UINT32 frames_needed = (buffer_frames_ > padding) ? (buffer_frames_ - padding) : 0;
    if (frames_needed == 0) continue;

    BYTE* render_buf = nullptr;
    hr = render_->GetBuffer(frames_needed, &render_buf);
    if (FAILED(hr) || render_buf == nullptr) continue;

    size_t bytes_needed = frames_needed * bytes_per_frame_;
    size_t bytes_filled = 0;

    // 1. Pull from ring buffer first
    {
      std::lock_guard<std::mutex> lock(ring_mu_);
      if (ring_count_ > 0) {
        size_t to_read = std::min(bytes_needed, ring_count_);
        size_t first = std::min(to_read, ring_buffer_.size() - ring_head_);
        memcpy(render_buf, ring_buffer_.data() + ring_head_, first);
        size_t second = to_read - first;
        if (second > 0) {
          memcpy(render_buf + first, ring_buffer_.data(), second);
        }
        ring_head_ = (ring_head_ + to_read) % ring_buffer_.size();
        ring_count_ -= to_read;
        bytes_filled += to_read;
      }
    }

    // 2. If space remains, pull from OpenSL ES queued buffers
    std::vector<std::pair<std::function<void(void*)>, void*>> completed_callbacks;
    if (bytes_filled < bytes_needed) {
      std::lock_guard<std::mutex> lock(opensl_mu_);
      while (bytes_filled < bytes_needed && !opensl_queue_.empty()) {
        auto& item = opensl_queue_.front();
        size_t item_avail = item.size_bytes - item.consumed_bytes;
        size_t needed = bytes_needed - bytes_filled;
        size_t copy_bytes = std::min(needed, item_avail);

        memcpy(render_buf + bytes_filled, static_cast<const uint8_t*>(item.data) + item.consumed_bytes,
               copy_bytes);
        item.consumed_bytes += copy_bytes;
        bytes_filled += copy_bytes;

        if (item.consumed_bytes >= item.size_bytes) {
          if (item.callback) completed_callbacks.emplace_back(item.callback, item.context);
          opensl_queue_.pop_front();
        }
      }
    }

    // 3. Fill remaining with silence
    DWORD flags = 0;
    if (bytes_filled < bytes_needed) {
      memset(render_buf + bytes_filled, 0, bytes_needed - bytes_filled);
      if (bytes_filled == 0 && !running_) {
        flags |= AUDCLNT_BUFFERFLAGS_SILENT;
      }
    }

    render_->ReleaseBuffer(frames_needed, flags);
    ring_cv_.notify_all();

    // Trigger OpenSL callbacks outside locks
    for (auto& cb : completed_callbacks) {
      cb.first(cb.second);
    }
  }

  if (avrt_handle != nullptr) {
    AvRevertMmThreadCharacteristics(avrt_handle);
  }
}

std::unique_ptr<WasapiAudioSink> CreateWasapiAudioSink(const WasapiSinkParams& params,
                                                       std::string* error) {
  auto sink = std::make_unique<WasapiAudioSink>();
  if (!sink->Initialize(params, error)) return nullptr;
  return sink;
}

std::unique_ptr<Sink> OpenLowLatencyWasapiSink(const std::u16string& app_name,
                                              std::string* error) {
  WasapiSinkParams params;
  params.app_name = app_name;
  params.sample_rate = 48000;
  params.channels = 2;
  params.format = WasapiSampleFormat::kFloat32;
  params.low_latency = true;
  return CreateWasapiAudioSink(params, error);
}

}  // namespace gesher::media
