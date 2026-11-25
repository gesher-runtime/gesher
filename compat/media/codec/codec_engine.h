// Gesher media: shared C++ MediaCodec engine (compat/media/codec/codec_engine.h).
//
// JNI-free core shared by android.media.MediaCodec (compat/media/jni/media_codec_jni.cc)
// and NDK AMediaCodec (compat/media/ndk/media_codec.cc). Owns the slot buffers,
// worker thread, VideoDecoder/AudioDecoder/VideoEncoder/AudioEncoder lifecycle,
// and synchronous/asynchronous buffer queues.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../encode/encoder.h"
#include "../player/decoder.h"
#include "../player/extractor.h"
#include "../video/video_decoder.h"

struct ANativeWindow;

namespace gesher::media {

inline constexpr int kCodecInfoTryAgainLater = -1;
inline constexpr int kCodecInfoOutputFormatChanged = -2;
inline constexpr int kCodecInfoOutputBuffersChanged = -3;

inline constexpr uint32_t kCodecFlagKeyFrame = 1;
inline constexpr uint32_t kCodecFlagCodecConfig = 2;
inline constexpr uint32_t kCodecFlagEndOfStream = 4;

inline constexpr int kCodecColorFormatYuv420Planar = 19;
inline constexpr int kCodecColorFormatYuv420SemiPlanar = 21;
inline constexpr int kCodecColorFormatYuv420Flexible = 0x7F420888;
inline constexpr int kCodecColorFormatSurface = 0x7F000789;

inline constexpr int kCodecSlots = 8;

struct CodecEntry {
  std::string name;
  std::string mime;
  bool video = false;
  bool hardware = false;
  int max_width = 0;
  int max_height = 0;
  bool encoder = false;
};

struct CodecBufferInfo {
  int32_t offset = 0;
  int32_t size = 0;
  int64_t time_us = 0;
  uint32_t flags = 0;
};

struct CodecMetrics {
  int32_t width = 0, height = 0, rotation = 0;
  int32_t sample_rate = 0, channel_count = 0;
  int32_t bitrate = 0;
};

// Optional hook for ImageReader surfaces (JNI-only).
using SurfaceDeliverHook = std::function<int(const VideoFrame& frame, int64_t timestamp_ns)>;

// JNI-free input surface source for video encoders.
class InputFrameSource {
 public:
  using FrameCallback =
      std::function<void(const uint32_t* rgba, int width, int height, int64_t timestamp_ns)>;

  virtual ~InputFrameSource() = default;
  virtual void SetCallback(FrameCallback cb) = 0;
  virtual void Abandon() = 0;
  virtual ANativeWindow* NativeWindow() = 0;
};

// Asynchronous notifications invoked without holding CodecEngine::mu_.
struct CodecCallbacks {
  std::function<void(int index)> on_input_available;
  std::function<void(int index, const CodecBufferInfo& info)> on_output_available;
  std::function<void(const Format& format)> on_output_format_changed;
  std::function<void(int error, int action, const std::string& message)> on_error;
  std::function<void(int64_t media_time_us, int64_t system_nano)> on_frame_rendered;
};

struct CodecSlot {
  std::vector<uint8_t> mem;
  bool client = false;  // dequeued by the app
  int offset = 0, size = 0;
  int64_t time_us = 0;
  uint32_t flags = 0;
  std::shared_ptr<VideoFrame> frame;        // video outputs
  std::unique_ptr<uint8_t[]> deferred_mem;  // CPU access after removing a Surface
};

class CodecEngine : public std::enable_shared_from_this<CodecEngine> {
 public:
  explicit CodecEngine(const CodecEntry& entry);
  ~CodecEngine();

  static const std::vector<CodecEntry>& Codecs();
  static const CodecEntry* FindByName(const std::string& name);
  static const CodecEntry* FindByType(const std::string& mime, bool encoder);

  const CodecEntry& entry() const { return entry_; }
  const VideoEncoderParams& enc_params() const { return enc_params_; }
  const AudioEncoderParams& audio_params() const { return audio_params_; }

  void Release();
  bool Configure(Format format, ANativeWindow* window, SurfaceDeliverHook reader_hook,
                 std::string* error, int* status = nullptr);
  void SetSurface(ANativeWindow* window, SurfaceDeliverHook reader_hook, int* status = nullptr);
  bool has_surface() const { return window_ != nullptr || static_cast<bool>(reader_hook_); }

  // Checks configured/idle encoder state before creating an InputFrameSource.
  bool PrepareInputSurface(int* width, int* height, std::string* error);
  void AttachInputSource(std::unique_ptr<InputFrameSource> source,
                         std::function<void()> on_retire_old = nullptr);
  void OnSurfaceFrame(const uint32_t* rgba, int w, int h, int64_t ts_ns);
  bool SignalEndOfInputStream(std::string* error);

  bool SetParameters(const Format& parameters);
  bool SetVideoScalingMode(int mode);
  bool GetMetrics(std::optional<CodecMetrics>* out_metrics);
  bool IsReleasedOrFatal();

  void SetAsync(bool async, CodecCallbacks callbacks = {});
  void EnableFrameRendered(bool enable, std::function<void(int64_t, int64_t)> cb = {});

  bool Start(std::string* error);
  void Stop(int* status = nullptr);
  void Reset();
  void Flush(int* status = nullptr);

  int DequeueInput(int64_t timeout_us, int* status = nullptr);
  bool QueueInput(int index, int64_t offset, int64_t size, int64_t time_us, uint32_t flags,
                  std::string* error, int* status = nullptr);
  int DequeueOutput(CodecBufferInfo* info, int64_t timeout_us, int* status = nullptr);
  bool ReleaseOutput(int index, bool render, bool has_timestamp, int64_t time_ns,
                     std::string* error, int* release_status = nullptr);

  uint8_t* GetInputBuffer(int index, size_t* out_size, int* status = nullptr);
  uint8_t* GetOutputBuffer(int index, size_t* out_size, bool* out_oom = nullptr,
                           int* status = nullptr, int* out_data_size = nullptr);

  // For JNI MediaCodec.getImage().
  bool GetVideoImageSlot(int index, bool input, uint8_t** data, size_t* size, int* width,
                         int* height, int64_t* time_us, bool* out_oom, int* status = nullptr);

  bool GetInputFormat(Format* out, int* status = nullptr);
  bool GetOutputFormat(Format* out, int* status = nullptr);

  // Acquires lifecycle_mu_ for external input-surface swap synchronization.
  std::mutex& lifecycle_mutex() { return lifecycle_mu_; }

 private:
  void StopLocked();
  void StopWorker();
  void SetSurfaceLocked(ANativeWindow* window, SurfaceDeliverHook reader_hook);
  bool InWork(int index) const;
  Format DecoderOutputFormatLocked() const;
  Format EncoderOutputFormatLocked() const;
  void BeginMetricsLocked();
  bool ConfigureEncoder(const Format& format, std::string* error);
  bool EnsureDecoder(std::string* error);
  bool EnsureEncoder(std::string* error);
  bool InputFrame(const uint8_t* p, size_t n, int64_t time_us, VideoFrame* f) const;
  void Run();
  void RunEncoder();
  void Fail(std::unique_lock<std::mutex>& lock, const std::string& error);
  int FreeOutput(std::unique_lock<std::mutex>& lock);
  void OutputFormatIs(std::unique_lock<std::mutex>& lock, int width, int height);
  void EncoderFormatIs(std::unique_lock<std::mutex>& lock, const EncodedPacket& config);
  void Publish(std::unique_lock<std::mutex>& lock, int index);
  bool EmitFrame(std::unique_lock<std::mutex>& lock, std::shared_ptr<VideoFrame> f);
  void EmitPcm(std::unique_lock<std::mutex>& lock, const std::vector<uint8_t>& pcm,
               int64_t time_us);
  void EmitEos(std::unique_lock<std::mutex>& lock, int64_t time_us);
  bool EmitPacket(std::unique_lock<std::mutex>& lock, const EncodedPacket& pkt);
  void RetireSurfaceFailure(uint64_t execution_generation, const std::string& error);

  CodecEntry entry_;
  std::mutex lifecycle_mu_;
  std::mutex mu_;
  std::condition_variable cv_, done_cv_;
  std::thread worker_;
  bool worker_running_ = false;
  bool released_ = false;
  uint64_t wait_generation_ = 0;
  uint64_t execution_generation_ = 0;
  uint64_t fatal_generation_ = 0;
  bool fatal_ = false;
  bool quit_ = false;
  bool configured_ = false, started_ = false, flushed_ = false, async_ = false;
  bool flush_requested_ = false;
  bool failed_ = false;
  CodecCallbacks callbacks_;
  std::function<void(int64_t, int64_t)> frame_rendered_cb_;
  std::optional<CodecMetrics> metrics_;
  std::atomic<bool> frame_rendered_{false};
  Format input_format_;
  ANativeWindow* window_ = nullptr;
  SurfaceDeliverHook reader_hook_;
  int video_scaling_mode_ = 1;
  CodecSlot inputs_[kCodecSlots];
  CodecSlot outputs_[kCodecSlots];
  std::deque<int> work_;
  std::deque<int> ready_;
  std::vector<std::vector<uint8_t>> config_;
  std::vector<uint8_t> pending_config_;
  std::unique_ptr<AudioDecoder> audio_;
  std::unique_ptr<VideoDecoder> video_;
  PcmInfo pcm_;
  bool out_float_ = false;
  bool output_known_ = false, format_pending_ = false;
  int out_width_ = 0, out_height_ = 0;
  VideoEncoderParams enc_params_;
  AudioEncoderParams audio_params_;
  int color_format_ = kCodecColorFormatYuv420Flexible;
  bool in_float_ = false;
  std::unique_ptr<VideoEncoder> video_enc_;
  std::unique_ptr<AudioEncoder> audio_enc_;
  std::unique_ptr<InputFrameSource> input_source_;
  std::deque<std::shared_ptr<VideoFrame>> surface_frames_;
  bool surface_eos_ = false, enc_eos_sent_ = false, force_key_ = false;
  std::optional<int> target_bitrate_;
  std::optional<int> pending_bitrate_;
  std::vector<std::vector<uint8_t>> csd_;
  int64_t last_out_us_ = 0;
};

}  // namespace gesher::media
