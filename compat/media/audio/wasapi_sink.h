// Gesher media: low-latency event-driven WASAPI audio sink (sink.h).
#pragma once

#undef NOGDI
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "sink.h"

namespace gesher::media {

enum class WasapiSampleFormat {
  kPcm16,     // 16-bit signed integer PCM
  kFloat32,   // 32-bit IEEE float
};

struct WasapiSinkParams {
  uint32_t sample_rate = 48000;         // 44100 or 48000 Hz
  uint32_t channels = 2;                // 2 (Stereo)
  WasapiSampleFormat format = WasapiSampleFormat::kFloat32;
  bool low_latency = true;              // Query IAudioClient3 for low-latency engine period
  uint32_t target_latency_ms = 10;      // Fallback buffer period
  std::u16string app_name;
  int stream_group = 0;                 // Android stream volume group (0 = Media)
};

// Formats helper functions supporting WAVEFORMATEX and WAVEFORMATEXTENSIBLE
// for PCM 16-bit and IEEE Float 32-bit (44100Hz and 48000Hz stereo).
bool BuildWaveFormatEx(uint32_t rate, uint32_t channels, WasapiSampleFormat format,
                       WAVEFORMATEX* out_wf);
bool BuildWaveFormatExtensible(uint32_t rate, uint32_t channels, WasapiSampleFormat format,
                               WAVEFORMATEXTENSIBLE* out_wfe);

// Queued OpenSL ES buffer item
struct OpenSLBufferItem {
  const void* data = nullptr;
  size_t size_bytes = 0;
  size_t consumed_bytes = 0;
  std::function<void(void*)> callback;
  void* context = nullptr;
};

// WASAPI shared mode sink with low-latency event-driven buffering.
class WasapiAudioSink : public Sink {
 public:
  WasapiAudioSink();
  ~WasapiAudioSink() override;

  // Initializes WASAPI audio client in shared mode with low latency (or IAudioClient3)
  bool Initialize(const WasapiSinkParams& params, std::string* error);
  void Close();

  // Sink interface implementation (compat/media/audio/sink.h)
  uint32_t rate() const override { return rate_; }
  uint32_t channels() const override { return channels_; }
  uint32_t latency_frames() const override;
  std::string Describe() const override;
  std::string DeviceName() const override { return device_name_; }
  int DeviceType() const override { return device_type_; }
  bool Tick(Mixer* mixer) override;
  void OnVolumesChanged() override;

  // Playback control
  bool Start();
  void Stop();
  void Pause();
  void Flush();
  void Reset();
  bool IsRunning() const { return running_; }

  // Thread-safe buffer feeding into the event-driven WASAPI queue
  size_t WriteFrames(const void* data, size_t frame_count, bool blocking = false);
  size_t WriteBytes(const void* data, size_t byte_count, bool blocking = false);

  // --- Android AudioTrack Native Sink Connection ---
  ssize_t WriteAudioTrack(const void* data, size_t byte_count, bool is_float, bool blocking);
  void SetAudioTrackVolume(float left, float right);

  // --- Android OpenSL ES Buffer Queue Connection ---
  bool EnqueueOpenSLBuffer(const void* buffer, size_t size_bytes,
                           std::function<void(void*)> callback, void* context);
  void ClearOpenSLQueue();
  size_t GetOpenSLQueueDepth() const;

  // Query state
  bool UsingAudioClient3() const { return using_client3_; }
  uint32_t buffer_frames() const { return buffer_frames_; }
  uint32_t period_frames() const { return period_frames_; }
  WasapiSampleFormat sample_format() const { return params_.format; }
  size_t bytes_per_frame() const { return bytes_per_frame_; }

 private:
  void FeedLoop();
  bool InitAudioClient(std::string* error);
  void ReadDeviceProperties();
  void ApplyVolume();

  WasapiSinkParams params_;
  std::u16string app_name_;
  std::string device_name_ = "Windows Default Audio Output";
  int device_type_ = 0x2;

  uint32_t rate_ = 48000;
  uint32_t channels_ = 2;
  size_t bytes_per_sample_ = 4;
  size_t bytes_per_frame_ = 8;
  uint32_t buffer_frames_ = 0;
  uint32_t period_frames_ = 0;
  uint32_t latency_extra_frames_ = 0;

  bool using_client3_ = false;
  std::atomic<bool> running_{false};
  std::atomic<bool> quit_{false};

  IMMDeviceEnumerator* enumerator_ = nullptr;
  IMMDevice* device_ = nullptr;
  IAudioClient* client_ = nullptr;
  IAudioRenderClient* render_ = nullptr;
  ISimpleAudioVolume* simple_volume_ = nullptr;
  IAudioSessionControl* session_control_ = nullptr;

  HANDLE buffer_event_ = nullptr;
  HANDLE stop_event_ = nullptr;
  std::thread feeder_thread_;

  WAVEFORMATEXTENSIBLE format_ext_ = {};
  WAVEFORMATEX format_ex_ = {};
  bool use_extensible_ = true;

  // Thread-safe ring buffer for feeding loop
  std::mutex ring_mu_;
  std::condition_variable ring_cv_;
  std::vector<uint8_t> ring_buffer_;
  size_t ring_head_ = 0;
  size_t ring_tail_ = 0;
  size_t ring_count_ = 0;

  // Thread-safe OpenSL ES queue
  mutable std::mutex opensl_mu_;
  std::deque<OpenSLBufferItem> opensl_queue_;

  float volume_left_ = 1.0f;
  float volume_right_ = 1.0f;
};

// Factory functions
std::unique_ptr<WasapiAudioSink> CreateWasapiAudioSink(const WasapiSinkParams& params,
                                                       std::string* error);
std::unique_ptr<Sink> OpenLowLatencyWasapiSink(const std::u16string& app_name,
                                              std::string* error);

}  // namespace gesher::media
