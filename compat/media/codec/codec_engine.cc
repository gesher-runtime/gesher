// Gesher media: shared C++ MediaCodec engine (compat/media/codec/codec_engine.cc).

#include "codec_engine.h"

#include <gesher/clocks.h>
#include <gesher/perf_trace.h>
#include <media/NdkMediaFormat.h>
#include <utils/Errors.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <limits>
#include <cmath>
#include <new>
#include <utility>

#include "../audio/log.h"
#include "../encode/codec_config.h"
#include "../video/yuv_rgba.h"

struct ANativeWindow;
extern "C" {
void ANativeWindow_acquire(ANativeWindow* window);
void ANativeWindow_release(ANativeWindow* window);
}

namespace gesher::media {

namespace {

std::string ShortName(const std::string& mime) {
  static const std::map<std::string, std::string> names = {
      {"video/avc", "avc"},           {"video/hevc", "hevc"},
      {"video/x-vnd.on2.vp8", "vp8"}, {"video/x-vnd.on2.vp9", "vp9"},
      {"video/av01", "av1"},          {"video/mp4v-es", "mpeg4"},
      {"video/3gpp", "h263"},         {"audio/mpeg", "mp3"},
      {"audio/mp4a-latm", "aac"},     {"audio/vorbis", "vorbis"},
      {"audio/opus", "opus"},         {"audio/raw", "raw"},
      {"audio/g711-mlaw", "g711.mlaw"}, {"audio/g711-alaw", "g711.alaw"},
  };
  auto it = names.find(mime);
  return it != names.end() ? it->second : mime;
}

void CopyOutputFramePixels(const VideoFrame& f, uint8_t* d) {
  const int w = f.visible_width(), h = f.visible_height();
  const int cw = (w + 1) / 2, ch = (h + 1) / 2;
  for (int y = 0; y < h; y++) {
    memcpy(d + static_cast<size_t>(y) * w,
           f.y() + static_cast<size_t>(f.crop_top + y) * f.stride_y + f.crop_left,
           static_cast<size_t>(w));
  }
  uint8_t* du = d + static_cast<size_t>(w) * h;
  uint8_t* dv = du + static_cast<size_t>(cw) * ch;
  for (int y = 0; y < ch; y++) {
    const size_t row = static_cast<size_t>(f.crop_top / 2 + y) * f.stride_uv + f.crop_left / 2;
    memcpy(du + static_cast<size_t>(y) * cw, f.u() + row, static_cast<size_t>(cw));
    memcpy(dv + static_cast<size_t>(y) * cw, f.v() + row, static_cast<size_t>(cw));
  }
}

size_t OutputFrameBytes(const VideoFrame& f) {
  const int w = f.visible_width(), h = f.visible_height();
  return static_cast<size_t>(w) * h +
         2 * static_cast<size_t>((w + 1) / 2) * ((h + 1) / 2);
}

void CopyOutputFrameToBuffer(CodecSlot& slot) {
  const VideoFrame& f = *slot.frame;
  const size_t size = OutputFrameBytes(f);
  if (slot.mem.size() < size) slot.mem.assign(size, 0);
  CopyOutputFramePixels(f, slot.mem.data());
}

uint8_t* CpuOutputBytes(CodecSlot& slot, bool* out_oom) {
  if (out_oom != nullptr) *out_oom = false;
  if (!slot.mem.empty()) return slot.mem.data();
  if (slot.frame == nullptr) return nullptr;
  if (slot.deferred_mem == nullptr) {
    slot.deferred_mem.reset(new (std::nothrow) uint8_t[OutputFrameBytes(*slot.frame)]);
    if (slot.deferred_mem == nullptr) {
      if (out_oom != nullptr) *out_oom = true;
      return nullptr;
    }
    CopyOutputFramePixels(*slot.frame, slot.deferred_mem.get());
  }
  return slot.deferred_mem.get();
}

bool InputCapacity(const Format& format, bool video, bool encoder, int width, int height,
                   size_t* capacity, std::string* error) {
  int32_t requested = 0;
  if (format.GetInt32("max-input-size", &requested) && requested > 0) {
    *capacity = static_cast<size_t>(requested);
    return true;
  }
  int64_t bytes = 1 << 16;
  if (video) {
    bytes = 1 << 20;
    if (width > 0 && height > 0) {
      const int64_t w = width, h = height;
      const int64_t yuv = w * h + 2 * ((w + 1) / 2) * ((h + 1) / 2);
      bytes = encoder ? yuv + 64 : std::max<int64_t>(yuv, 1 << 20);
    }
  }
  if (bytes > std::numeric_limits<int32_t>::max()) {
    if (error) *error = "codec input buffer dimensions exceed the Android buffer range";
    return false;
  }
  *capacity = static_cast<size_t>(bytes);
  return true;
}

}  // namespace

const std::vector<CodecEntry>& CodecEngine::Codecs() {
  static const std::vector<CodecEntry> codecs = [] {
    std::vector<CodecEntry> all;
    for (const VideoCodecInfo& v : VideoDecoder::Available()) {
      all.push_back({"c2.gesher." + ShortName(v.mime) + ".decoder", v.mime, true, v.hardware,
                     v.max_width, v.max_height, false});
    }
    for (const char* mime : {"audio/mp4a-latm", "audio/mpeg", "audio/vorbis", "audio/opus",
                             "audio/raw", "audio/g711-mlaw", "audio/g711-alaw"}) {
      if (AudioDecoder::Supports(mime)) {
        all.push_back({"c2.gesher." + ShortName(mime) + ".decoder", mime, false, false, 0, 0,
                       false});
      }
    }
    for (const EncoderInfo& e : AvailableEncoders()) {
      all.push_back({"c2.gesher." + ShortName(e.mime) + ".encoder", e.mime, e.video, false,
                     e.video ? 1920 : 0, e.video ? 1088 : 0, true});
    }
    return all;
  }();
  return codecs;
}

const CodecEntry* CodecEngine::FindByName(const std::string& name) {
  for (const CodecEntry& c : Codecs()) {
    if (c.name == name) return &c;
  }
  return nullptr;
}

const CodecEntry* CodecEngine::FindByType(const std::string& mime, bool encoder) {
  for (const CodecEntry& c : Codecs()) {
    if (c.mime == mime && c.encoder == encoder) return &c;
  }
  return nullptr;
}

CodecEngine::CodecEngine(const CodecEntry& entry) : entry_(entry) {}

CodecEngine::~CodecEngine() {
  Release();
  if (window_ != nullptr) ANativeWindow_release(window_);
}

void CodecEngine::Release() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (released_) return;
    released_ = true;
    configured_ = false;
  }
  StopLocked();
  ANativeWindow* window = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    window = window_;
    window_ = nullptr;
    reader_hook_ = nullptr;
    callbacks_ = {};
    frame_rendered_cb_ = {};
  }
  if (window != nullptr) ANativeWindow_release(window);
}

bool CodecEngine::Configure(Format format, ANativeWindow* window, SurfaceDeliverHook reader_hook,
                            std::string* error, int* status) {
  if (status != nullptr) *status = android::BAD_VALUE;
  std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
  std::lock_guard<std::mutex> lock(mu_);
  if (released_ || fatal_ || configured_) {
    if (error != nullptr) *error = "configure() requires an idle codec; reset after a fatal error";
    if (status != nullptr) *status = android::INVALID_OPERATION;
    return false;
  }
  if (format.get() == nullptr) {
    if (error != nullptr) *error = "configure() requires a non-null format";
    return false;
  }
  std::string mime = format.Mime();
  if (!mime.empty() && mime != entry_.mime) {
    if (error != nullptr) *error = "format " + mime + " for " + entry_.name;
    return false;
  }
  if (mime.empty()) AMediaFormat_setString(format.get(), "mime", entry_.mime.c_str());
  if (entry_.encoder && !ConfigureEncoder(format, error)) return false;
  input_format_ = std::move(format);
  video_scaling_mode_ = 1;
  if (entry_.encoder) {
    BeginMetricsLocked();
    configured_ = true;
    if (status != nullptr) *status = android::OK;
    return true;
  }
  SetSurfaceLocked(window, std::move(reader_hook));
  int32_t w = 0, h = 0;
  input_format_.GetInt32("width", &w);
  input_format_.GetInt32("height", &h);
  size_t max_input = 0;
  if (!InputCapacity(input_format_, entry_.video, false, w, h, &max_input, error)) return false;
  for (CodecSlot& s : inputs_) {
    s.mem.assign(max_input, 0);
    s.client = false;
  }
  for (CodecSlot& s : outputs_) s = CodecSlot();
  BeginMetricsLocked();
  configured_ = true;
  if (status != nullptr) *status = android::OK;
  return true;
}

void CodecEngine::SetSurfaceLocked(ANativeWindow* window, SurfaceDeliverHook reader_hook) {
  if (window != nullptr) ANativeWindow_acquire(window);
  if (window_ != nullptr) ANativeWindow_release(window_);
  window_ = window;
  reader_hook_ = (window == nullptr) ? std::move(reader_hook) : nullptr;
}

void CodecEngine::SetSurface(ANativeWindow* window, SurfaceDeliverHook reader_hook, int* status) {
  if (status != nullptr) *status = android::OK;
  std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
  std::lock_guard<std::mutex> lock(mu_);
  if (released_ || fatal_) {
    if (status != nullptr) *status = android::INVALID_OPERATION;
    return;
  }
  SetSurfaceLocked(window, std::move(reader_hook));
}

bool CodecEngine::PrepareInputSurface(int* width, int* height, std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!entry_.encoder || !entry_.video) {
    if (error != nullptr) *error = "createInputSurface() is for video encoders";
    return false;
  }
  if (released_ || fatal_ || !configured_ || started_) {
    if (error != nullptr)
      *error = "createInputSurface() must be called after configure() and before start()";
    return false;
  }
  if (width != nullptr) *width = enc_params_.width;
  if (height != nullptr) *height = enc_params_.height;
  return true;
}

void CodecEngine::SetOutputSurface(ANativeWindow* window, int* status) {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
  std::lock_guard<std::mutex> lock(mu_);
  if (window == nullptr) {
    *status = android::BAD_VALUE;
    return;
  }
  if (released_ || fatal_ || !configured_ || !entry_.video || entry_.encoder || !has_surface()) {
    *status = android::INVALID_OPERATION;
    return;
  }
  SetSurfaceLocked(window, {});
  *status = android::OK;
}

ANativeWindow* CodecEngine::InputWindow() {
  std::lock_guard<std::mutex> lock(mu_);
  return input_source_ != nullptr ? input_source_->NativeWindow() : nullptr;
}

void CodecEngine::AttachInputSource(std::unique_ptr<InputFrameSource> source,
                                    std::function<void()> on_retire_old) {
  if (source != nullptr) {
    std::weak_ptr<CodecEngine> weak = weak_from_this();
    source->SetCallback([weak](const uint32_t* rgba, int w, int h, int64_t ts_ns) {
      if (auto self = weak.lock()) self->OnSurfaceFrame(rgba, w, h, ts_ns);
    });
  }
  if (input_source_ != nullptr) input_source_->Abandon();
  std::unique_ptr<InputFrameSource> retired;
  {
    std::lock_guard<std::mutex> lock(mu_);
    retired = std::move(input_source_);
    input_source_ = std::move(source);
  }
  if (on_retire_old) on_retire_old();
}

bool CodecEngine::SignalEndOfInputStream(std::string* error, int* status) {
  std::lock_guard<std::mutex> lock(mu_);
  if (status != nullptr) *status = android::INVALID_OPERATION;
  if (released_ || fatal_ || !started_ || flushed_ || flush_requested_ || input_source_ == nullptr) {
    if (error != nullptr) *error = "signalEndOfInputStream() requires an executing surface encoder";
    return false;
  }
  if (failed_) {
    if (status != nullptr) *status = android::UNKNOWN_ERROR;
    if (error != nullptr) *error = "signalEndOfInputStream() after codec failure";
    return false;
  }
  surface_eos_ = true;
  cv_.notify_all();
  if (status != nullptr) *status = android::OK;
  return true;
}

bool CodecEngine::SetParameters(const Format& parameters) {
  int32_t bitrate = 0, sync = 0;
  parameters.GetInt32("video-bitrate", &bitrate);
  const bool request_sync = parameters.GetInt32("request-sync", &sync);
  std::lock_guard<std::mutex> lock(mu_);
  if (released_ || fatal_ || !started_ || flushed_ || flush_requested_ || failed_) return false;
  if (configured_ && entry_.video && entry_.encoder && !enc_eos_sent_) {
    if (request_sync) force_key_ = true;
    if (bitrate > 0) pending_bitrate_ = bitrate;
  }
  return true;
}

bool CodecEngine::SetVideoScalingMode(int mode) {
  std::lock_guard<std::mutex> lock(mu_);
  if (released_) return false;
  if (has_surface()) video_scaling_mode_ = mode;
  return true;
}

bool CodecEngine::GetMetrics(std::optional<CodecMetrics>* out_metrics) {
  std::lock_guard<std::mutex> lock(mu_);
  if (released_) return false;
  if (out_metrics != nullptr) *out_metrics = metrics_;
  return true;
}

bool CodecEngine::IsReleasedOrFatal() {
  std::lock_guard<std::mutex> lock(mu_);
  return released_ || fatal_;
}

bool CodecEngine::IsCallbackCurrent(uint64_t generation) {
  std::lock_guard<std::mutex> lock(mu_);
  // A fatal error still has to reach the application in its current epoch.
  return !released_ && generation == wait_generation_;
}

bool CodecEngine::EnqueueCallbackIfCurrent(uint64_t generation,
                                          const std::function<void()>& enqueue) {
  std::lock_guard<std::mutex> lock(mu_);
  if (released_ || generation != wait_generation_) return false;
  enqueue();
  return true;
}

bool CodecEngine::SetAsync(bool async, CodecCallbacks callbacks) {
  std::lock_guard<std::mutex> lock(mu_);
  if (released_ || fatal_ || started_) return false;
  async_ = async;
  callbacks_ = async ? std::move(callbacks) : CodecCallbacks{};
  return true;
}

void CodecEngine::EnableFrameRendered(bool enable, std::function<void(int64_t, int64_t)> cb) {
  std::lock_guard<std::mutex> lock(mu_);
  frame_rendered_ = enable;
  if (cb) frame_rendered_cb_ = std::move(cb);
}

bool CodecEngine::Start(std::string* error) {
  std::unique_lock<std::mutex> lifecycle(lifecycle_mu_);
  std::vector<int> notify_inputs;
  std::function<void(uint64_t, int)> input_cb;
  uint64_t callback_generation = 0;
  {
    std::unique_lock<std::mutex> lock(mu_);
    if (released_ || fatal_ || !configured_) {
      if (error != nullptr) *error = "start() before configure()";
      return false;
    }
    if (started_ && !flushed_) return true;
    flushed_ = false;
    started_ = true;
    if (!worker_.joinable()) {
      ++execution_generation_;
      quit_ = false;
      worker_running_ = true;
      worker_ = std::thread([this] { Run(); });
    }
    if (async_ && input_source_ == nullptr) {
      input_cb = callbacks_.on_input_available;
      callback_generation = wait_generation_;
      for (int i = 0; i < kCodecSlots; i++) {
        if (!inputs_[i].client) {
          inputs_[i].client = true;
          notify_inputs.push_back(i);
        }
      }
    }
    cv_.notify_all();
  }
  lifecycle.unlock();
  if (input_cb) {
    for (int i : notify_inputs) input_cb(callback_generation, i);
  }
  return true;
}

void CodecEngine::Stop(int* status) {
  if (status != nullptr) *status = android::OK;
  std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (released_) {
      if (status != nullptr) *status = android::INVALID_OPERATION;
      return;
    }
    if (fatal_) {
      if (status != nullptr) *status = android::INVALID_OPERATION;
      return;
    }
  }
  StopLocked();
}

void CodecEngine::StopLocked() {
  StopWorker();
  if (input_source_ != nullptr) input_source_->Abandon();
  std::unique_ptr<InputFrameSource> retired_input;
  std::lock_guard<std::mutex> lock(mu_);
  retired_input = std::move(input_source_);
  configured_ = false;
  video_scaling_mode_ = 1;
  video_enc_.reset();
  audio_enc_.reset();
  surface_frames_.clear();
  surface_eos_ = false;
  enc_eos_sent_ = false;
  force_key_ = false;
  pending_bitrate_.reset();
  last_out_us_ = 0;
  csd_.clear();
  work_.clear();
  ready_.clear();
  for (CodecSlot& s : inputs_) s.client = false;
  for (CodecSlot& s : outputs_) {
    s.client = false;
    s.frame.reset();
    s.format.reset();
  }
  audio_.reset();
  video_.reset();
  output_known_ = false;
  output_format_.reset();
  format_pending_ = false;
  config_.clear();
  pending_config_.clear();
  flush_requested_ = false;
  flushed_ = false;
}

void CodecEngine::Reset() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
  StopLocked();
  std::lock_guard<std::mutex> lock(mu_);
  target_bitrate_.reset();
  configured_ = false;
  fatal_ = false;
  failed_ = false;
}

void CodecEngine::Flush(int* status) {
  if (status != nullptr) *status = android::OK;
  std::unique_lock<std::mutex> lock(mu_);
  if (released_ || fatal_ || !started_ || !worker_running_) {
    if (status != nullptr) *status = android::INVALID_OPERATION;
    return;
  }
  const uint64_t fatal_generation = fatal_generation_;
  const uint64_t generation = ++wait_generation_;
  flush_requested_ = true;
  work_.clear();
  cv_.notify_all();
  done_cv_.notify_all();
  done_cv_.wait(lock, [this, generation] {
    return generation != wait_generation_ || quit_ || !worker_running_ || !flush_requested_;
  });
  if (generation != wait_generation_ || quit_ || !started_ || released_) {
    if (status != nullptr) {
      *status =
          fatal_generation != fatal_generation_ ? android::UNKNOWN_ERROR : android::INVALID_OPERATION;
    }
    return;
  }
  ready_.clear();
  for (CodecSlot& s : inputs_) s.client = false;
  for (CodecSlot& s : outputs_) {
    s.client = false;
    s.frame.reset();
  }
  if (async_) flushed_ = true;
}

int CodecEngine::DequeueInput(int64_t timeout_us, int* status) {
  if (status != nullptr) *status = android::OK;
  std::unique_lock<std::mutex> lock(mu_);
  const uint64_t generation = wait_generation_;
  if (failed_) {
    if (status != nullptr) *status = android::UNKNOWN_ERROR;
    return kCodecInfoTryAgainLater;
  }
  if (released_ || !started_ || async_ || input_source_ != nullptr) {
    if (status != nullptr) *status = android::INVALID_OPERATION;
    return kCodecInfoTryAgainLater;
  }
  auto free_slot = [this] {
    for (int i = 0; i < kCodecSlots; i++) {
      if (!inputs_[i].client && !InWork(i)) return i;
    }
    return -1;
  };
  int i = -1;
  auto ready = [&] {
    return generation != wait_generation_ || quit_ || !started_ || failed_ || (i = free_slot()) >= 0;
  };
  if (timeout_us < 0) {
    cv_.wait(lock, ready);
  } else {
    cv_.wait_for(lock, std::chrono::microseconds(timeout_us), ready);
  }
  if (generation != wait_generation_ || quit_ || !started_ || released_) {
    if (status != nullptr) *status = android::INVALID_OPERATION;
    return kCodecInfoTryAgainLater;
  }
  if (failed_) {
    if (status != nullptr) *status = android::UNKNOWN_ERROR;
    return kCodecInfoTryAgainLater;
  }
  if (i < 0) return kCodecInfoTryAgainLater;
  inputs_[i].client = true;
  return i;
}

bool CodecEngine::QueueInput(int index, int64_t offset, int64_t size, int64_t time_us,
                             uint32_t flags, std::string* error, int* status) {
  std::lock_guard<std::mutex> lock(mu_);
  if (failed_) {
    if (error) *error = "codec failed; stop and configure it before queuing more input";
    if (status) *status = android::UNKNOWN_ERROR;
    return false;
  }
  if (released_ || !started_ || index < 0 || index >= kCodecSlots || !inputs_[index].client) {
    if (error != nullptr) *error = "invalid input buffer index " + std::to_string(index);
    if (status != nullptr) *status = android::INVALID_OPERATION;
    return false;
  }
  CodecSlot& s = inputs_[index];
  if (offset < 0 || size < 0 ||
      static_cast<uint64_t>(offset) + static_cast<uint64_t>(size) > s.mem.size()) {
    if (error != nullptr) *error = "input range outside the buffer";
    if (status != nullptr) *status = android::BAD_VALUE;
    return false;
  }
  if (flags & (kCodecFlagPartialFrame | kCodecFlagDecodeOnly)) {
    if (error != nullptr) *error = "partial-frame and decode-only inputs are unsupported";
    if (status != nullptr) *status = -ENOTSUP;
    return false;
  }
  s.client = false;
  s.offset = static_cast<int>(offset);
  s.size = static_cast<int>(size);
  s.time_us = time_us;
  s.flags = flags;
  work_.push_back(index);
  cv_.notify_all();
  if (status != nullptr) *status = android::OK;
  return true;
}

int CodecEngine::DequeueOutput(CodecBufferInfo* info, int64_t timeout_us, int* status) {
  if (status != nullptr) *status = android::OK;
  std::unique_lock<std::mutex> lock(mu_);
  const uint64_t generation = wait_generation_;
  if (released_ || !started_ || async_) {
    if (status != nullptr) *status = android::INVALID_OPERATION;
    return kCodecInfoTryAgainLater;
  }
  auto ready = [this, generation] {
    return generation != wait_generation_ || quit_ || !started_ || format_pending_ ||
           !ready_.empty() || failed_;
  };
  if (timeout_us < 0) {
    cv_.wait(lock, ready);
  } else {
    cv_.wait_for(lock, std::chrono::microseconds(timeout_us), ready);
  }
  if (generation != wait_generation_ || quit_ || !started_ || released_) {
    if (status != nullptr) *status = android::INVALID_OPERATION;
    return kCodecInfoTryAgainLater;
  }
  if (format_pending_) {
    format_pending_ = false;
    return kCodecInfoOutputFormatChanged;
  }
  if (ready_.empty()) {
    if (failed_ && status != nullptr) *status = android::UNKNOWN_ERROR;
    return kCodecInfoTryAgainLater;
  }
  int i = ready_.front();
  ready_.pop_front();
  CodecSlot& s = outputs_[i];
  s.client = true;
  if (info != nullptr) {
    info->offset = 0;
    info->size = s.size;
    info->time_us = s.time_us;
    info->flags = s.flags;
  }
  return i;
}

bool CodecEngine::ReleaseOutput(int index, bool render, bool has_timestamp, int64_t time_ns,
                                std::string* error, int* release_status) {
  if (release_status != nullptr) *release_status = android::OK;
  std::shared_ptr<VideoFrame> frame;
  std::unique_ptr<ANativeWindow, decltype(&ANativeWindow_release)> window(
      nullptr, ANativeWindow_release);
  SurfaceDeliverHook reader_hook;
  int64_t time_us = 0;
  int video_scaling_mode = 1;
  uint64_t execution_generation = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (released_ || !started_) {
      if (error != nullptr) *error = "releaseOutputBuffer() requires an executing codec";
      if (release_status != nullptr) *release_status = android::INVALID_OPERATION;
      return false;
    }
    if (index < 0 || index >= kCodecSlots) {
      if (error != nullptr) *error = "invalid output buffer index " + std::to_string(index);
      if (release_status != nullptr) *release_status = -ERANGE;
      return false;
    }
    if (!outputs_[index].client) {
      if (error != nullptr) *error = "output buffer is not owned by the client";
      if (release_status != nullptr) *release_status = -EACCES;
      return false;
    }
    CodecSlot& s = outputs_[index];
    s.client = false;
    frame = std::move(s.frame);
    time_us = s.time_us;
    video_scaling_mode = video_scaling_mode_;
    execution_generation = execution_generation_;
    if (render && window_ != nullptr) {
      ANativeWindow_acquire(window_);
      window.reset(window_);
    }
    if (render) reader_hook = reader_hook_;
    cv_.notify_all();
  }
  if (frame == nullptr || (window == nullptr && !reader_hook)) return true;
  const int64_t timestamp_ns =
      has_timestamp ? time_ns : static_cast<int64_t>(static_cast<uint64_t>(time_us) * 1000);
  static gesher::perf::Site render_site("MediaCodec: render a frame into its Surface");
  const int64_t render_start = gesher::perf::Now();
  const int delivery_status =
      window != nullptr ? RenderFrame(window.get(), *frame, timestamp_ns, video_scaling_mode)
                        : reader_hook(*frame, timestamp_ns);
  if (window != nullptr) render_site.Add(gesher::perf::Now() - render_start);
  if (delivery_status == android::NO_INIT) {
    if (error != nullptr) *error = "rendering to an abandoned output surface";
    RetireSurfaceFailure(execution_generation,
                         error != nullptr ? *error : "rendering to an abandoned output surface");
    if (release_status != nullptr) *release_status = delivery_status;
    return false;
  }
  if (delivery_status == android::OK) {
    std::function<void(int64_t, int64_t)> frame_cb;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (execution_generation == execution_generation_ && started_ && !fatal_ && frame_rendered_) {
        frame_cb = frame_rendered_cb_;
      }
    }
    if (frame_cb) frame_cb(time_us, gesher::MonotonicNanos());
  }
  return true;
}

void CodecEngine::RetireSurfaceFailure(uint64_t execution_generation, const std::string& error) {
  std::unique_lock<std::mutex> lifecycle(lifecycle_mu_);
  bool notify_error = false;
  std::function<void(uint64_t, int, int, const std::string&)> error_cb;
  uint64_t callback_generation = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (released_ || fatal_ || execution_generation != execution_generation_) return;
    fatal_ = true;
    ++fatal_generation_;
    configured_ = false;
    started_ = false;
    quit_ = true;
    notify_error = async_ && !flush_requested_;
    error_cb = callbacks_.on_error;
    async_ = false;
    ++wait_generation_;
    cv_.notify_all();
    done_cv_.notify_all();
  }
  StopLocked();
  ANativeWindow* window = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    window = window_;
    window_ = nullptr;
    reader_hook_ = nullptr;
    callback_generation = wait_generation_;
  }
  lifecycle.unlock();
  if (window != nullptr) ANativeWindow_release(window);
  if (notify_error && error_cb) error_cb(callback_generation, android::UNKNOWN_ERROR, 0, error);
}

uint8_t* CodecEngine::GetInputBuffer(int index, size_t* out_size, int* status) {
  if (out_size != nullptr) *out_size = 0;
  std::lock_guard<std::mutex> lock(mu_);
  if (released_ || fatal_) {
    if (status != nullptr) *status = android::INVALID_OPERATION;
    return nullptr;
  }
  if (index < 0 || index >= kCodecSlots) {
    if (status != nullptr) *status = android::BAD_VALUE;
    return nullptr;
  }
  CodecSlot& s = inputs_[index];
  if (s.mem.empty()) {
    if (status != nullptr) *status = android::INVALID_OPERATION;
    return nullptr;
  }
  if (out_size != nullptr) *out_size = s.mem.size();
  if (status != nullptr) *status = android::OK;
  return s.mem.data();
}

bool CodecEngine::OwnsBuffer(int index, bool input) {
  std::lock_guard<std::mutex> lock(mu_);
  return !released_ && !fatal_ && started_ && index >= 0 && index < kCodecSlots &&
         (input ? inputs_[index].client : outputs_[index].client);
}

uint8_t* CodecEngine::GetOutputBuffer(int index, size_t* out_size, bool* out_oom, int* status,
                                      int* out_data_size) {
  if (out_size != nullptr) *out_size = 0;
  if (out_oom != nullptr) *out_oom = false;
  if (out_data_size != nullptr) *out_data_size = 0;
  std::lock_guard<std::mutex> lock(mu_);
  if (released_ || fatal_) {
    if (status != nullptr) *status = android::INVALID_OPERATION;
    return nullptr;
  }
  if (index < 0 || index >= kCodecSlots || has_surface()) {
    if (status != nullptr) *status = android::BAD_VALUE;
    return nullptr;
  }
  CodecSlot& s = outputs_[index];
  uint8_t* data = CpuOutputBytes(s, out_oom);
  if (data == nullptr) {
    if (status != nullptr) *status = (out_oom && *out_oom) ? -ENOMEM : android::INVALID_OPERATION;
    return nullptr;
  }
  if (out_size != nullptr) {
    *out_size = s.mem.empty() ? OutputFrameBytes(*s.frame) : s.mem.size();
  }
  if (out_data_size != nullptr) *out_data_size = s.size;
  if (status != nullptr) *status = android::OK;
  return data;
}

bool CodecEngine::GetVideoImageSlot(int index, bool input, uint8_t** data, size_t* size,
                                      int* width, int* height, int64_t* time_us, bool* out_oom,
                                      int* status) {
  if (out_oom != nullptr) *out_oom = false;
  std::lock_guard<std::mutex> lock(mu_);
  if (released_ || fatal_) {
    if (status != nullptr) *status = android::INVALID_OPERATION;
    return false;
  }
  if (!entry_.video || index < 0 || index >= kCodecSlots) {
    if (status != nullptr) *status = android::BAD_VALUE;
    return false;
  }
  if (input) {
    if (!entry_.encoder || input_source_ != nullptr) return false;
    CodecSlot& s = inputs_[index];
    const int w = enc_params_.width;
    const int h = enc_params_.height;
    const size_t need =
        static_cast<size_t>(w) * h + 2 * static_cast<size_t>((w + 1) / 2) * ((h + 1) / 2);
    if (s.mem.size() < need) return false;
    *data = s.mem.data();
    *size = need;
    *width = w;
    *height = h;
    *time_us = 0;
    if (status != nullptr) *status = android::OK;
    return true;
  }
  if (has_surface() || entry_.encoder) return false;
  CodecSlot& s = outputs_[index];
  if (s.size == 0) return false;
  uint8_t* ptr = CpuOutputBytes(s, out_oom);
  if (ptr == nullptr) return false;
  *data = ptr;
  *size = static_cast<size_t>(s.size);
  *width = out_width_;
  *height = out_height_;
  *time_us = s.time_us;
  if (status != nullptr) *status = android::OK;
  return true;
}

bool CodecEngine::GetInputFormat(Format* out, int* status) {
  std::lock_guard<std::mutex> lock(mu_);
  if (released_ || fatal_) {
    if (status != nullptr) *status = android::INVALID_OPERATION;
    return false;
  }
  Format copy;
  if (input_format_.get() != nullptr) AMediaFormat_copy(copy.get(), input_format_.get());
  *out = std::move(copy);
  if (status != nullptr) *status = android::OK;
  return true;
}

bool CodecEngine::GetOutputFormat(Format* out, int* status) {
  std::lock_guard<std::mutex> lock(mu_);
  if (released_ || fatal_) {
    if (status != nullptr) *status = android::INVALID_OPERATION;
    return false;
  }
  *out = entry_.encoder ? EncoderOutputFormatLocked() : DecoderOutputFormatLocked();
  if (status != nullptr) *status = android::OK;
  return true;
}

bool CodecEngine::GetBufferFormat(int index, Format* out) {
  std::lock_guard<std::mutex> lock(mu_);
  if (released_ || fatal_ || !started_ || index < 0 || index >= kCodecSlots ||
      !outputs_[index].client || !outputs_[index].format) return false;
  return AMediaFormat_copy(out->get(), outputs_[index].format->get()) == AMEDIA_OK;
}

bool CodecEngine::InWork(int index) const {
  return std::find(work_.begin(), work_.end(), index) != work_.end();
}

Format CodecEngine::DecoderOutputFormatLocked() const {
  Format f;
  if (entry_.video) {
    AMediaFormat_setString(f.get(), "mime", "video/raw");
    AMediaFormat_setInt32(f.get(), "width", out_width_);
    AMediaFormat_setInt32(f.get(), "height", out_height_);
    AMediaFormat_setInt32(f.get(), "stride", out_width_);
    AMediaFormat_setInt32(f.get(), "slice-height", out_height_);
    AMediaFormat_setInt32(f.get(), "crop-left", 0);
    AMediaFormat_setInt32(f.get(), "crop-top", 0);
    AMediaFormat_setInt32(f.get(), "crop-right", out_width_ - 1);
    AMediaFormat_setInt32(f.get(), "crop-bottom", out_height_ - 1);
    AMediaFormat_setInt32(
        f.get(), "color-format",
        has_surface() ? kCodecColorFormatSurface : kCodecColorFormatYuv420Planar);
  } else {
    AMediaFormat_setString(f.get(), "mime", "audio/raw");
    AMediaFormat_setInt32(f.get(), "sample-rate", static_cast<int32_t>(pcm_.rate));
    AMediaFormat_setInt32(f.get(), "channel-count", static_cast<int32_t>(pcm_.channels));
    AMediaFormat_setInt32(f.get(), "pcm-encoding", out_float_ ? 4 : 2);
  }
  int64_t duration = 0;
  if (input_format_.GetInt64("durationUs", &duration)) {
    AMediaFormat_setInt64(f.get(), "durationUs", duration);
  }
  return f;
}

Format CodecEngine::EncoderOutputFormatLocked() const {
  Format f;
  AMediaFormat_setString(f.get(), "mime", entry_.mime.c_str());
  if (entry_.video) {
    AMediaFormat_setInt32(f.get(), "width", enc_params_.width);
    AMediaFormat_setInt32(f.get(), "height", enc_params_.height);
    AMediaFormat_setInt32(f.get(), "frame-rate", enc_params_.frame_rate);
    AMediaFormat_setInt32(f.get(), "bitrate", target_bitrate_.value_or(enc_params_.bitrate));
  } else {
    AMediaFormat_setInt32(f.get(), "sample-rate", static_cast<int32_t>(audio_params_.sample_rate));
    AMediaFormat_setInt32(f.get(), "channel-count", static_cast<int32_t>(audio_params_.channels));
    AMediaFormat_setInt32(f.get(), "bitrate", audio_params_.bitrate);
    if (entry_.mime == "audio/mp4a-latm") {
      AMediaFormat_setInt32(f.get(), "aac-profile", audio_params_.aac_profile);
    }
  }
  for (size_t i = 0; i < csd_.size(); i++) {
    const std::string key = "csd-" + std::to_string(i);
    AMediaFormat_setBuffer(f.get(), key.c_str(), csd_[i].data(), csd_[i].size());
  }
  return f;
}

void CodecEngine::StopWorker() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    started_ = false;
    quit_ = true;
    ++execution_generation_;
    ++wait_generation_;
    cv_.notify_all();
    done_cv_.notify_all();
  }
  // JNI posts to Java's event queue; native callbacks use CodecCallbackQueue.
  // Neither adapter executes application code on this worker.
  if (worker_.joinable()) worker_.join();
  std::lock_guard<std::mutex> lock(mu_);
  worker_running_ = false;
  flush_requested_ = false;
  done_cv_.notify_all();
}

bool CodecEngine::EnsureDecoder(std::string* error) {
  if (audio_ != nullptr || video_ != nullptr) return true;
  for (size_t i = 0; i < config_.size(); i++) {
    std::string key = "csd-" + std::to_string(i);
    std::vector<uint8_t> have;
    if (!input_format_.GetBuffer(key.c_str(), &have)) {
      AMediaFormat_setBuffer(input_format_.get(), key.c_str(), config_[i].data(), config_[i].size());
    }
  }
  if (entry_.video) {
    video_ = VideoDecoder::Create(input_format_, error);
    return video_ != nullptr;
  }
  audio_ = AudioDecoder::Create(input_format_, error);
  if (audio_ == nullptr) return false;
  pcm_ = audio_->info();
  int32_t encoding = 2;
  input_format_.GetInt32("pcm-encoding", &encoding);
  out_float_ = encoding == 4 && pcm_.format == PcmFormat::kFloat;
  return true;
}

void CodecEngine::Run() {
  if (entry_.encoder) {
    RunEncoder();
    return;
  }
  std::unique_lock<std::mutex> lock(mu_);
  for (;;) {
    cv_.wait(lock, [this] {
      return quit_ || flush_requested_ || (started_ && !flushed_ && !failed_ && !work_.empty());
    });
    if (quit_) return;
    if (flush_requested_) {
      if (audio_ != nullptr) audio_->Flush(0);
      if (video_ != nullptr) video_->Flush();
      failed_ = false;
      flush_requested_ = false;
      done_cv_.notify_all();
      continue;
    }
    int index = work_.front();
    work_.pop_front();
    CodecSlot& in = inputs_[index];
    Sample sample;
    sample.data.assign(in.mem.begin() + in.offset, in.mem.begin() + in.offset + in.size);
    sample.time_us = in.time_us;
    uint32_t flags = in.flags;
    std::function<void(uint64_t, int)> input_cb;
    const uint64_t callback_generation = wait_generation_;
    if (async_ && !flushed_) {
      in.client = true;
      input_cb = callbacks_.on_input_available;
    }
    cv_.notify_all();
    if (input_cb) {
      lock.unlock();
      input_cb(callback_generation, index);
      lock.lock();
      if (quit_ || flush_requested_) continue;
    }
    if (flags & kCodecFlagCodecConfig) {
      if (video_ != nullptr && !sample.data.empty()) {
        pending_config_.insert(pending_config_.end(), sample.data.begin(), sample.data.end());
      } else if (!sample.data.empty()) {
        config_.push_back(sample.data);
      }
      continue;
    }
    std::string error;
    if (!EnsureDecoder(&error)) {
      Fail(lock, error);
      continue;
    }
    if (!pending_config_.empty()) {
      sample.data.insert(sample.data.begin(), pending_config_.begin(), pending_config_.end());
      pending_config_.clear();
    }
    bool eos = (flags & kCodecFlagEndOfStream) != 0;
    lock.unlock();
    std::vector<uint8_t> pcm;
    std::vector<VideoFrame> frames;
    bool ok = true;
    if (!sample.data.empty()) {
      static gesher::perf::Site video_site("MediaCodec: decode a video sample");
      static gesher::perf::Site audio_site("MediaCodec: decode an audio sample");
      const int64_t start = gesher::perf::Now();
      ok = video_ != nullptr ? video_->Decode(sample, &frames) : audio_->Decode(sample, &pcm);
      (video_ != nullptr ? video_site : audio_site).Add(gesher::perf::Now() - start);
    }
    if (ok && eos) {
      if (video_ != nullptr) {
        video_->Drain(&frames);
      } else {
        audio_->Drain(&pcm);
      }
    }
    lock.lock();
    if (quit_ || flush_requested_) continue;
    if (!ok) {
      Fail(lock, entry_.name + ": the decoder failed");
      continue;
    }
    if (video_ != nullptr) {
      for (VideoFrame& f : frames) {
        if (!EmitFrame(lock, std::make_shared<VideoFrame>(std::move(f)))) break;
      }
    } else if (!pcm.empty()) {
      EmitPcm(lock, pcm, sample.time_us);
    }
    if (eos && !failed_) EmitEos(lock, sample.time_us);
  }
}

void CodecEngine::Fail(std::unique_lock<std::mutex>& lock, const std::string& error) {
  MEDIA_LOGE("MediaCodec %s: %s", entry_.name.c_str(), error.c_str());
  failed_ = true;
  const bool async = async_;
  auto cb = callbacks_.on_error;
  const uint64_t callback_generation = wait_generation_;
  cv_.notify_all();
  if (async && cb) {
    lock.unlock();
    cb(callback_generation, -1, 2, error);
    lock.lock();
  }
}

void CodecEngine::BeginMetricsLocked() {
  CodecMetrics metrics;
  target_bitrate_.reset();
  if (entry_.video) {
    input_format_.GetInt32("width", &metrics.width);
    input_format_.GetInt32("height", &metrics.height);
    input_format_.GetInt32("rotation-degrees", &metrics.rotation);
    if (entry_.encoder) {
      metrics.bitrate = std::max(enc_params_.bitrate, 64000);
      target_bitrate_ = metrics.bitrate;
    }
  } else if (entry_.encoder) {
    metrics.sample_rate = static_cast<int32_t>(audio_params_.sample_rate);
    metrics.channel_count = static_cast<int32_t>(audio_params_.channels);
    metrics.bitrate = audio_params_.bitrate;
  } else {
    input_format_.GetInt32("sample-rate", &metrics.sample_rate);
    input_format_.GetInt32("channel-count", &metrics.channel_count);
  }
  metrics_ = metrics;
  pending_bitrate_.reset();
  force_key_ = false;
  failed_ = false;
}

int CodecEngine::FreeOutput(std::unique_lock<std::mutex>& lock) {
  int found = -1;
  cv_.wait(lock, [&] {
    if (quit_ || flush_requested_) return true;
    for (int i = 0; i < kCodecSlots; i++) {
      if (!outputs_[i].client && std::find(ready_.begin(), ready_.end(), i) == ready_.end()) {
        found = i;
        return true;
      }
    }
    return false;
  });
  return quit_ || flush_requested_ ? -1 : found;
}

void CodecEngine::OutputFormatIs(std::unique_lock<std::mutex>& lock, int width, int height) {
  if (output_known_ && (!entry_.video || (width == out_width_ && height == out_height_))) return;
  output_known_ = true;
  out_width_ = width;
  out_height_ = height;
  output_format_ = std::make_shared<Format>(DecoderOutputFormatLocked());
  if (async_) {
    auto cb = callbacks_.on_output_format_changed;
    const uint64_t callback_generation = wait_generation_;
    Format fmt = DecoderOutputFormatLocked();
    cv_.notify_all();
    if (cb) {
      lock.unlock();
      cb(callback_generation, fmt);
      lock.lock();
    }
  } else {
    format_pending_ = true;
    cv_.notify_all();
  }
}

void CodecEngine::Publish(std::unique_lock<std::mutex>& lock, int index) {
  outputs_[index].format = output_format_;
  if (async_) {
    CodecSlot& s = outputs_[index];
    s.client = true;
    CodecBufferInfo info{0, s.size, s.time_us, s.flags};
    auto cb = callbacks_.on_output_available;
    const uint64_t callback_generation = wait_generation_;
    cv_.notify_all();
    if (cb) {
      lock.unlock();
      cb(callback_generation, index, info);
      lock.lock();
    }
  } else {
    ready_.push_back(index);
    cv_.notify_all();
  }
}

bool CodecEngine::EmitFrame(std::unique_lock<std::mutex>& lock, std::shared_ptr<VideoFrame> f) {
  const int w = f->visible_width(), h = f->visible_height();
  metrics_->width = w;
  metrics_->height = h;
  OutputFormatIs(lock, w, h);
  if (quit_ || flush_requested_) return false;
  int i = FreeOutput(lock);
  if (i < 0) return false;
  CodecSlot& s = outputs_[i];
  const int cw = (w + 1) / 2, ch = (h + 1) / 2;
  const size_t size = static_cast<size_t>(w) * h + 2 * static_cast<size_t>(cw) * ch;
  s.size = static_cast<int>(size);
  s.offset = 0;
  s.time_us = f->time_us;
  s.flags = 0;
  s.frame = std::move(f);
  s.deferred_mem.reset();
  if (has_surface()) {
    std::vector<uint8_t>().swap(s.mem);
  } else {
    CopyOutputFrameToBuffer(s);
  }
  Publish(lock, i);
  return !quit_ && !flush_requested_;
}

void CodecEngine::EmitPcm(std::unique_lock<std::mutex>& lock, const std::vector<uint8_t>& pcm,
                          int64_t time_us) {
  metrics_->sample_rate = static_cast<int32_t>(pcm_.rate);
  metrics_->channel_count = static_cast<int32_t>(pcm_.channels);
  OutputFormatIs(lock, 0, 0);
  if (quit_ || flush_requested_) return;
  std::vector<uint8_t> converted;
  const std::vector<uint8_t>* data = &pcm;
  if (pcm_.format == PcmFormat::kFloat && !out_float_) {
    size_t n = pcm.size() / 4;
    converted.resize(n * 2);
    const float* src = reinterpret_cast<const float*>(pcm.data());
    int16_t* dst = reinterpret_cast<int16_t*>(converted.data());
    for (size_t k = 0; k < n; k++) {
      float v = std::isfinite(src[k]) ? std::clamp(src[k], -1.0f, 1.0f) : 0.0f;
      dst[k] = static_cast<int16_t>(v * 32767.0f);
    }
    data = &converted;
  }
  const size_t frame_bytes = (out_float_ ? size_t{4} : size_t{2}) * pcm_.channels;
  if (frame_bytes == 0 || frame_bytes > 64 * 1024) {
    Fail(lock, "invalid decoded PCM channel count");
    return;
  }
  const size_t chunk = (64 * 1024 / frame_bytes) * frame_bytes;
  size_t done = 0;
  while (done < data->size()) {
    int i = FreeOutput(lock);
    if (i < 0) return;
    CodecSlot& s = outputs_[i];
    size_t n = std::min(chunk, data->size() - done);
    if (s.mem.size() < chunk) s.mem.assign(chunk, 0);
    memcpy(s.mem.data(), data->data() + done, n);
    s.size = static_cast<int>(n);
    s.offset = 0;
    s.time_us =
        time_us +
        (pcm_.rate > 0 ? static_cast<int64_t>(done / frame_bytes) * 1000000 / pcm_.rate : 0);
    s.flags = 0;
    s.frame.reset();
    done += n;
    Publish(lock, i);
    if (quit_ || flush_requested_) return;
  }
}

void CodecEngine::EmitEos(std::unique_lock<std::mutex>& lock, int64_t time_us) {
  if (!output_known_ && !entry_.encoder) {
    OutputFormatIs(lock, entry_.video ? std::max(out_width_, 1) : 0, std::max(out_height_, 1));
    if (quit_ || flush_requested_) return;
  }
  int i = FreeOutput(lock);
  if (i < 0) return;
  CodecSlot& s = outputs_[i];
  if (s.mem.empty()) s.mem.assign(16, 0);
  s.size = 0;
  s.offset = 0;
  s.time_us = time_us;
  s.flags = kCodecFlagEndOfStream;
  s.frame.reset();
  s.deferred_mem.reset();
  Publish(lock, i);
}

bool CodecEngine::ConfigureEncoder(const Format& format, std::string* error) {
  int32_t v = 0;
  float fv = 0;
  if (entry_.video) {
    VideoEncoderParams p;
    format.GetInt32("width", &p.width);
    format.GetInt32("height", &p.height);
    if (format.GetInt32("frame-rate", &v) && v > 0) {
      p.frame_rate = v;
    } else if (format.GetFloat("frame-rate", &fv) && fv > 0) {
      p.frame_rate = static_cast<int>(fv + 0.5f);
    }
    if (format.GetInt32("bitrate", &v) && v > 0) p.bitrate = v;
    if (format.GetInt32("i-frame-interval", &v)) {
      p.i_frame_interval_s = static_cast<float>(v);
    } else if (format.GetFloat("i-frame-interval", &fv)) {
      p.i_frame_interval_s = fv;
    }
    if (format.GetInt32("profile", &v)) p.profile = v;
    if (p.width <= 0 || p.height <= 0) {
      if (error != nullptr) *error = entry_.name + ": the format has no width and height";
      return false;
    }
    if (VideoEncoder::Create(entry_.mime, p, error) == nullptr) return false;
    color_format_ = kCodecColorFormatYuv420Flexible;
    format.GetInt32("color-format", &color_format_);
    enc_params_ = p;
  } else {
    AudioEncoderParams p;
    if (format.GetInt32("sample-rate", &v) && v > 0) p.sample_rate = static_cast<uint32_t>(v);
    if (format.GetInt32("channel-count", &v) && v > 0) p.channels = static_cast<uint32_t>(v);
    if (format.GetInt32("bitrate", &v) && v > 0) p.bitrate = v;
    if (format.GetInt32("aac-profile", &v)) p.aac_profile = v;
    auto encoder = AudioEncoder::Create(entry_.mime, p, error);
    if (encoder == nullptr) return false;
    p.bitrate = encoder->ConfiguredBitrate();
    int32_t encoding = 2;
    format.GetInt32("pcm-encoding", &encoding);
    in_float_ = encoding == 4;
    audio_params_ = p;
  }
  size_t max_input = 0;
  if (!InputCapacity(format, entry_.video, true, enc_params_.width, enc_params_.height,
                     &max_input, error)) return false;
  for (CodecSlot& s : inputs_) {
    s.mem.assign(max_input, 0);
    s.client = false;
  }
  for (CodecSlot& s : outputs_) s = CodecSlot();
  return true;
}

void CodecEngine::OnSurfaceFrame(const uint32_t* rgba, int w, int h, int64_t ts_ns) {
  int width, height;
  uint64_t execution, flush;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_ || released_ || fatal_ || surface_eos_ || flushed_ || flush_requested_) return;
    width = enc_params_.width;
    height = enc_params_.height;
    execution = execution_generation_;
    flush = wait_generation_;
  }
  auto f = std::make_shared<VideoFrame>();
  f->Allocate(width, height);
  RgbaToI420(rgba, w, h, f.get());
  if (ts_ns <= 0) ts_ns = gesher::MonotonicNanos();
  f->time_us = ts_ns / 1000;
  std::lock_guard<std::mutex> lock(mu_);
  if (!started_ || released_ || fatal_ || surface_eos_ || flushed_ || flush_requested_ ||
      execution != execution_generation_ || flush != wait_generation_) {
    return;
  }
  while (surface_frames_.size() >= 8) surface_frames_.pop_front();
  surface_frames_.push_back(std::move(f));
  cv_.notify_all();
}

void CodecEngine::EncoderFormatIs(std::unique_lock<std::mutex>& lock, const EncodedPacket& config) {
  csd_.clear();
  if (entry_.mime == "video/avc") {
    for (auto [nal, len] : SplitAnnexB(config.data.data(), config.data.size())) {
      if (len == 0) continue;
      std::vector<uint8_t> unit = {0, 0, 0, 1};
      unit.insert(unit.end(), nal, nal + len);
      const int type = nal[0] & 0x1f;
      if (type == 7) csd_.insert(csd_.begin(), unit);
      if (type == 8) csd_.push_back(unit);
    }
  } else if (entry_.mime == "audio/opus") {
    csd_.push_back(config.data);
    const int64_t delay_ns = audio_enc_ != nullptr && audio_params_.sample_rate > 0
                                 ? static_cast<int64_t>(audio_enc_->delay()) * 1000000000 /
                                       audio_params_.sample_rate
                                 : 0;
    const int64_t preroll_ns = 80000000;
    std::vector<uint8_t> d(8), r(8);
    memcpy(d.data(), &delay_ns, 8);
    memcpy(r.data(), &preroll_ns, 8);
    csd_.push_back(d);
    csd_.push_back(r);
  } else {
    csd_.push_back(config.data);
  }
  output_known_ = true;
  output_format_ = std::make_shared<Format>(EncoderOutputFormatLocked());
  if (async_) {
    auto cb = callbacks_.on_output_format_changed;
    const uint64_t callback_generation = wait_generation_;
    Format fmt = EncoderOutputFormatLocked();
    cv_.notify_all();
    if (cb) {
      lock.unlock();
      cb(callback_generation, fmt);
      lock.lock();
    }
  } else {
    format_pending_ = true;
    cv_.notify_all();
  }
}

bool CodecEngine::EmitPacket(std::unique_lock<std::mutex>& lock, const EncodedPacket& pkt) {
  if (pkt.config) {
    EncoderFormatIs(lock, pkt);
    if (quit_ || flush_requested_) return false;
  }
  int i = FreeOutput(lock);
  if (i < 0) return false;
  CodecSlot& s = outputs_[i];
  if (s.mem.size() < pkt.data.size()) s.mem.assign(std::max<size_t>(pkt.data.size(), 4096), 0);
  memcpy(s.mem.data(), pkt.data.data(), pkt.data.size());
  s.size = static_cast<int>(pkt.data.size());
  s.offset = 0;
  s.time_us = pkt.time_us;
  s.flags = (pkt.config ? kCodecFlagCodecConfig : 0) |
            (pkt.key && !pkt.config ? kCodecFlagKeyFrame : 0);
  s.frame.reset();
  Publish(lock, i);
  return !quit_ && !flush_requested_;
}

bool CodecEngine::EnsureEncoder(std::string* error) {
  if (video_enc_ != nullptr || audio_enc_ != nullptr) return true;
  if (entry_.video) {
    video_enc_ = VideoEncoder::Create(entry_.mime, enc_params_, error);
    const int configured_bitrate = std::max(enc_params_.bitrate, 64000);
    if (video_enc_ && target_bitrate_ && *target_bitrate_ != configured_bitrate &&
        !video_enc_->SetBitrate(*target_bitrate_)) {
      target_bitrate_ = configured_bitrate;
    }
    return video_enc_ != nullptr;
  }
  audio_enc_ = AudioEncoder::Create(entry_.mime, audio_params_, error);
  return audio_enc_ != nullptr;
}

bool CodecEngine::InputFrame(const uint8_t* p, size_t n, int64_t time_us, VideoFrame* f) const {
  const int w = enc_params_.width, h = enc_params_.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
  const size_t need = static_cast<size_t>(w) * h + 2 * static_cast<size_t>(cw) * ch;
  if (n < need) return false;
  f->Allocate(w, h);
  f->time_us = time_us;
  if (color_format_ == kCodecColorFormatYuv420SemiPlanar) {
    Nv12ToI420(p, w, h, f);
  } else {
    memcpy(f->data.data(), p, need);
  }
  return true;
}

void CodecEngine::RunEncoder() {
  std::unique_lock<std::mutex> lock(mu_);
  for (;;) {
    cv_.wait(lock, [this] {
      return quit_ || flush_requested_ ||
             (started_ && !flushed_ && !failed_ && !enc_eos_sent_ &&
              (!work_.empty() || !surface_frames_.empty() || surface_eos_));
    });
    if (quit_) return;
    if (flush_requested_) {
      video_enc_.reset();
      audio_enc_.reset();
      surface_frames_.clear();
      pending_bitrate_.reset();
      force_key_ = false;
      failed_ = false;
      flush_requested_ = false;
      done_cv_.notify_all();
      continue;
    }
    std::string error;
    if (!EnsureEncoder(&error)) {
      work_.clear();
      surface_frames_.clear();
      Fail(lock, error);
      continue;
    }
    if (pending_bitrate_) {
      const int bitrate = *pending_bitrate_;
      pending_bitrate_.reset();
      if (target_bitrate_ != bitrate) {
        lock.unlock();
        const bool applied = video_enc_->SetBitrate(bitrate);
        lock.lock();
        if (applied) target_bitrate_ = bitrate;
        if (quit_ || flush_requested_) continue;
      }
    }
    std::vector<EncodedPacket> packets;
    bool ok = true, eos = false;
    int64_t eos_time = 0;
    if (!surface_frames_.empty()) {
      std::shared_ptr<VideoFrame> f = std::move(surface_frames_.front());
      surface_frames_.pop_front();
      const bool key = force_key_;
      force_key_ = false;
      lock.unlock();
      ok = video_enc_->Encode(*f, key, &packets);
      lock.lock();
      eos_time = f->time_us;
    } else if (!work_.empty()) {
      const int index = work_.front();
      work_.pop_front();
      CodecSlot& in = inputs_[index];
      std::vector<uint8_t> bytes(in.mem.begin() + in.offset, in.mem.begin() + in.offset + in.size);
      const int64_t time_us = in.time_us;
      const uint32_t flags = in.flags;
      std::function<void(uint64_t, int)> input_cb;
      const uint64_t callback_generation = wait_generation_;
      if (async_ && !flushed_) {
        in.client = true;
        input_cb = callbacks_.on_input_available;
      }
      cv_.notify_all();
      if (input_cb) {
        lock.unlock();
        input_cb(callback_generation, index);
        lock.lock();
        if (quit_ || flush_requested_) continue;
      }
      const bool key = force_key_;
      force_key_ = false;
      eos = (flags & kCodecFlagEndOfStream) != 0;
      eos_time = time_us;
      lock.unlock();
      if (!bytes.empty()) {
        if (video_enc_ != nullptr) {
          VideoFrame f;
          ok = InputFrame(bytes.data(), bytes.size(), time_us, &f) &&
               video_enc_->Encode(f, key, &packets);
        } else {
          std::vector<int16_t> pcm;
          if (in_float_) {
            const size_t count = bytes.size() / 4;
            pcm.resize(count);
            const float* fp = reinterpret_cast<const float*>(bytes.data());
            for (size_t k = 0; k < count; k++) {
              const float value = std::isfinite(fp[k]) ? std::clamp(fp[k], -1.0f, 1.0f) : 0.0f;
              pcm[k] = static_cast<int16_t>(value * 32767.0f);
            }
          } else {
            pcm.resize(bytes.size() / 2);
            memcpy(pcm.data(), bytes.data(), pcm.size() * 2);
          }
          ok = audio_enc_->Encode(pcm.data(), pcm.size() / audio_params_.channels, time_us,
                                  &packets);
        }
      }
      if (ok && eos) {
        ok = video_enc_ != nullptr ? video_enc_->Drain(&packets) : audio_enc_->Drain(&packets);
      }
      lock.lock();
    } else if (surface_eos_) {
      eos = true;
      lock.unlock();
      ok = video_enc_->Drain(&packets);
      lock.lock();
    }
    if (quit_ || flush_requested_) continue;
    if (!ok) {
      Fail(lock, entry_.name + ": the encoder failed");
      continue;
    }
    for (const EncodedPacket& pkt : packets) {
      if (!EmitPacket(lock, pkt)) break;
      last_out_us_ = std::max(last_out_us_, pkt.time_us);
    }
    if (!quit_ && !flush_requested_ && eos) {
      EmitEos(lock, std::max(eos_time, last_out_us_));
      enc_eos_sent_ = true;
    }
  }
}

}  // namespace gesher::media
