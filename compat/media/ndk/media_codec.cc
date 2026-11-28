// The NDK's MediaCodec (libmediandk.so) for loaded ELF code, backed by
// Gesher's shared CodecEngine (compat/media/codec/codec_engine.h).

#include "media_codec_abi.h"

#include <gesher/arm64_runtime.h>
#include <gesher/bionic_shim.h>
#include <media/NdkMediaFormat.h>
#include <utils/Errors.h>

#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <unordered_map>

#include "../codec/callback_queue.h"
#include "../codec/codec_engine.h"
#include "../surface/queue_surface.h"

extern "C" {
void ANativeWindow_acquire(ANativeWindow* window);
void ANativeWindow_release(ANativeWindow* window);

// libmediandk's parameter-key data objects (NdkMediaCodec.h, API 31+). An app
// imports each pointer object, not a function.
const char* AMEDIACODEC_KEY_HDR10_PLUS_INFO = "hdr10-plus-info";
const char* AMEDIACODEC_KEY_LOW_LATENCY = "low-latency";
const char* AMEDIACODEC_KEY_OFFSET_TIME = "time-offset-us";
const char* AMEDIACODEC_KEY_REQUEST_SYNC_FRAME = "request-sync";
const char* AMEDIACODEC_KEY_SUSPEND = "drop-input-frames";
const char* AMEDIACODEC_KEY_SUSPEND_TIME = "drop-start-time-us";
const char* AMEDIACODEC_KEY_VIDEO_BITRATE = "video-bitrate";
}

// Rendered-frame timing samples for AMediaCodec_setOnFrameRenderedCallback.
// Android delivers them on one NDK thread and may omit intermediate samples,
// so a slow callback coalesces to the latest sample instead of blocking the
// caller of releaseOutputBuffer.
struct NativeFrameRendered : std::enable_shared_from_this<NativeFrameRendered> {
  std::shared_ptr<gesher::media::CodecCallbackQueue> queue =
      gesher::media::CodecCallbackQueue::Create(gesher::bionic::EnterLoadedCode);
  AMediaCodec* codec = nullptr;
  gesher::media::codec_abi::OnFrameRendered callback = nullptr;
  void* userdata = nullptr;

  void Post(int64_t media_time_us, int64_t system_nanos) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      latest_ = Sample{media_time_us, system_nanos};
      if (scheduled_) return;
      scheduled_ = true;
    }
    auto self = shared_from_this();
    queue->Post([self] { self->Deliver(); });
  }

  void Discard() {
    std::lock_guard<std::mutex> lock(mu_);
    latest_.reset();
  }

 private:
  struct Sample {
    int64_t media_time_us;
    int64_t system_nanos;
  };

  void Deliver() {
    std::optional<Sample> sample;
    {
      std::lock_guard<std::mutex> lock(mu_);
      sample = std::exchange(latest_, std::nullopt);
      scheduled_ = false;
    }
    if (sample) callback(codec, userdata, sample->media_time_us, sample->system_nanos);
  }

  std::mutex mu_;
  std::optional<Sample> latest_;
  bool scheduled_ = false;
};

struct NativeCodecNotifications {
  std::shared_ptr<gesher::media::CodecCallbackQueue> queue =
      gesher::media::CodecCallbackQueue::Create(gesher::bionic::EnterLoadedCode);
  AMediaCodec* codec = nullptr;
  std::weak_ptr<gesher::media::CodecEngine> engine;
  gesher::media::codec_abi::OnAsyncNotifyCallback callback{};
  void* userdata = nullptr;

  bool IsCurrent(uint64_t generation) const {
    const auto owner = engine.lock();
    return owner && owner->IsCallbackCurrent(generation);
  }
};

struct AMediaCodec {
  std::shared_ptr<gesher::media::CodecEngine> engine;
  std::mutex async_mu;
  std::shared_ptr<NativeCodecNotifications> notifications;
  std::shared_ptr<NativeFrameRendered> frame_rendered;  // Guarded by async_mu.
};

namespace gesher::media {
namespace {

namespace abi = codec_abi;

template <auto Fn>
struct Sysv;

template <typename R, typename... A, R (*Fn)(A...)>
struct Sysv<Fn> {
  static R GESHER_CODEC_GUEST_ABI Call(A... args) { return Fn(args...); }
};

struct NdkInputState {
  std::recursive_mutex mu;
  uint64_t owner = 0, next_owner = 1;
  InputFrameSource::FrameCallback cb;
};

std::mutex gPersistentInputsMu;
std::unordered_map<ANativeWindow*, std::weak_ptr<NdkInputState>> gPersistentInputs;

// The registry contains only these producer objects, never an arbitrary
// SurfaceTexture/SurfaceView. A consumer lease retains the producer and state.
class NdkPersistentSurface final : public QueueSurface {
 public:
  NdkPersistentSurface() : QueueSurface(1, 1), state(std::make_shared<NdkInputState>()) {
    std::lock_guard<std::mutex> lock(gPersistentInputsMu);
    gPersistentInputs[this] = state;
  }
  ~NdkPersistentSurface() override {
    std::lock_guard<std::mutex> lock(gPersistentInputsMu);
    gPersistentInputs.erase(this);
  }
  std::shared_ptr<NdkInputState> state;
};

class NdkInputSource final : public InputFrameSource {
 public:
  NdkInputSource(android::sp<QueueSurface> surface, bool persistent,
                 std::shared_ptr<NdkInputState> state, uint64_t owner)
      : state_(std::move(state)), surface_(std::move(surface)),
        owner_(owner), persistent_(persistent) {}
  ~NdkInputSource() override { Abandon(); }

  void SetCallback(FrameCallback cb) override {
    std::lock_guard<std::recursive_mutex> lock(state_->mu);
    if (state_->owner != owner_) return;
    state_->cb = std::move(cb);
    QueueSurface* raw = surface_.get();
    std::shared_ptr<NdkInputState> s = state_;
    const uint64_t owner = owner_;
    surface_->SetListener([raw, s, owner] {
      std::lock_guard<std::recursive_mutex> lock(s->mu);
      if (s->owner != owner || !s->cb) return;
      FrameCallback fn = s->cb;
      std::shared_ptr<const SurfaceFrame> f = raw->Latest();
      if (f != nullptr && !f->pixels.empty()) {
        fn(f->pixels.data(), f->width, f->height, f->timestamp_ns);
      }
    });
  }

  void Abandon() override {
    std::lock_guard<std::recursive_mutex> lock(state_->mu);
    if (state_->owner != owner_) return;
    state_->owner = 0;
    state_->cb = nullptr;
    if (surface_ != nullptr) {
      if (persistent_) {
        surface_->SetListener(nullptr);
      } else {
        surface_->Abandon();
      }
    }
  }

  ANativeWindow* NativeWindow() override { return surface_.get(); }

 private:
  std::shared_ptr<NdkInputState> state_;
  android::sp<QueueSurface> surface_;
  const uint64_t owner_;
  const bool persistent_;
};

std::shared_ptr<NativeCodecNotifications> Notifications(AMediaCodec* codec) {
  std::lock_guard<std::mutex> lock(codec->async_mu);
  return codec->notifications;
}

std::shared_ptr<NativeFrameRendered> FrameRendered(AMediaCodec* codec) {
  std::lock_guard<std::mutex> lock(codec->async_mu);
  return codec->frame_rendered;
}

// Android 16's NdkMediaCodec translate_error reports codec state errors,
// including INVALID_OPERATION, as AMEDIA_ERROR_UNKNOWN. Gesher's own argument
// checks keep their specific parameter and unsupported-operation results.
media_status_t ToMediaStatus(int status) {
  if (status == android::OK) return AMEDIA_OK;
  if (status == android::BAD_VALUE) return AMEDIA_ERROR_INVALID_PARAMETER;
  if (status == -ENOTSUP) return AMEDIA_ERROR_UNSUPPORTED;
  return AMEDIA_ERROR_UNKNOWN;
}

// A callback in translated ARM64 guest code is called through its host entry.
template <class Callback>
bool ResolveGuestCallback(Callback* callback) {
  auto* address = reinterpret_cast<void*>(*callback);
  if (address == nullptr || !gesher::arm64::IsGuestCode(address)) return true;
  address = gesher::arm64::HostEntry(address);
  if (address == nullptr) return false;
  *callback = reinterpret_cast<Callback>(address);
  return true;
}

// As AMediaFormat_fromMsg does for a missing format, an unavailable codec
// format is an owned, empty AMediaFormat rather than null.
AMediaFormat* OwnedFormat(Format* format, bool available) {
  return available ? format->Release() : AMediaFormat_new();
}

AMediaCodec* CreateFromEntry(const CodecEntry* entry) {
  if (entry == nullptr) return nullptr;
  auto codec = std::unique_ptr<AMediaCodec>(new (std::nothrow) AMediaCodec);
  if (codec == nullptr) return nullptr;
  codec->engine = std::make_shared<CodecEngine>(*entry);
  return codec.release();
}

AMediaCodec* CreateCodecByName(const char* name) {
  if (name == nullptr) return nullptr;
  return CreateFromEntry(CodecEngine::FindByName(name));
}

AMediaCodec* CreateDecoderByType(const char* mime_type) {
  if (mime_type == nullptr) return nullptr;
  return CreateFromEntry(CodecEngine::FindByType(mime_type, false));
}

AMediaCodec* CreateEncoderByType(const char* mime_type) {
  if (mime_type == nullptr) return nullptr;
  return CreateFromEntry(CodecEngine::FindByType(mime_type, true));
}

media_status_t Delete(AMediaCodec* codec) {
  if (codec == nullptr) return AMEDIA_OK;
  const auto engine = codec->engine;
  const auto notifications = Notifications(codec);
  if (notifications) notifications->queue->Close();
  if (const auto frames = FrameRendered(codec)) frames->queue->Close();
  if (engine != nullptr) engine->Release();
  delete codec;
  return AMEDIA_OK;
}

media_status_t Configure(AMediaCodec* codec, const AMediaFormat* format, ANativeWindow* surface,
                         AMediaCrypto* crypto, uint32_t flags) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (format == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  if (crypto != nullptr) return AMEDIA_ERROR_UNSUPPORTED;
  const CodecEntry& entry = codec->engine->entry();
  const bool encode_flag = (flags & abi::kConfigureFlagEncode) != 0;
  if (encode_flag != entry.encoder) return AMEDIA_ERROR_INVALID_PARAMETER;
  // Only a video decoder renders into an output surface.
  if (surface != nullptr && (entry.encoder || !entry.video)) return AMEDIA_ERROR_INVALID_PARAMETER;

  Format copy;
  if (AMediaFormat_copy(copy.get(), const_cast<AMediaFormat*>(format)) != AMEDIA_OK) {
    return AMEDIA_ERROR_UNKNOWN;
  }
  // Bound the slot allocation before the shared worker multiplies dimensions
  // or reserves an app-controlled number of bytes. Current providers declare
  // no video dimensions beyond this bound.
  constexpr int32_t kMaxVideoDimension = 8192;
  constexpr int32_t kMaxInputCapacity = 64 * 1024 * 1024;
  int32_t max_input = 0;
  if (copy.GetInt32("max-input-size", &max_input) && max_input > kMaxInputCapacity) {
    return AMEDIA_ERROR_INVALID_PARAMETER;
  }
  if (entry.video) {
    int32_t width = 0, height = 0;
    if (!copy.GetInt32("width", &width) || !copy.GetInt32("height", &height) || width <= 0 ||
        height <= 0 || width > kMaxVideoDimension || height > kMaxVideoDimension) {
      return AMEDIA_ERROR_INVALID_PARAMETER;
    }
  }
  std::string error;
  int status = android::OK;
  if (!codec->engine->Configure(std::move(copy), surface, nullptr, &error, &status)) {
    return ToMediaStatus(status);
  }
  return AMEDIA_OK;
}

media_status_t Start(AMediaCodec* codec) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  // A native callback may release its codec before Start returns. Keep only
  // independent owners after notifications become runnable.
  const auto engine = codec->engine;
  const auto notifications = Notifications(codec);
  if (notifications) notifications->queue->Resume();
  std::string error;
  // NdkMediaCodec reports MediaCodec's state error as AMEDIA_ERROR_UNKNOWN.
  return engine->Start(&error) ? AMEDIA_OK : AMEDIA_ERROR_UNKNOWN;
}

media_status_t Stop(AMediaCodec* codec) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  const auto engine = codec->engine;
  const auto notifications = Notifications(codec);
  if (notifications) notifications->queue->Pause();
  if (const auto frames = FrameRendered(codec)) frames->Discard();
  int status = android::OK;
  engine->Stop(&status);
  return ToMediaStatus(status);
}

media_status_t Flush(AMediaCodec* codec) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  const auto engine = codec->engine;
  const auto notifications = Notifications(codec);
  if (notifications) notifications->queue->Pause();
  if (const auto frames = FrameRendered(codec)) frames->Discard();
  int status = android::OK;
  engine->Flush(&status);
  return ToMediaStatus(status);
}

// As in NdkMediaCodec, a rejected index leaves the caller's size unchanged.
uint8_t* GetInputBuffer(AMediaCodec* codec, size_t idx, size_t* out_size) {
  if (codec == nullptr || codec->engine == nullptr) return nullptr;
  if (idx >= kCodecSlots) return nullptr;
  if (!codec->engine->OwnsBuffer(static_cast<int>(idx), true)) return nullptr;
  size_t size = 0;
  uint8_t* data = codec->engine->GetInputBuffer(static_cast<int>(idx), &size);
  if (data != nullptr && out_size != nullptr) *out_size = size;
  return data;
}

uint8_t* GetOutputBuffer(AMediaCodec* codec, size_t idx, size_t* out_size) {
  if (codec == nullptr || codec->engine == nullptr) return nullptr;
  if (idx >= kCodecSlots) return nullptr;
  if (!codec->engine->OwnsBuffer(static_cast<int>(idx), false)) return nullptr;
  int data_size = 0;
  auto* data = codec->engine->GetOutputBuffer(static_cast<int>(idx), nullptr, nullptr, nullptr,
                                               &data_size);
  if (data && out_size) *out_size = static_cast<size_t>(data_size);
  return data;
}

abi::guest_ssize_t DequeueInputBuffer(AMediaCodec* codec, int64_t timeout_us) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  int status = android::OK;
  const int index = codec->engine->DequeueInput(timeout_us, &status);
  if (status != android::OK) return ToMediaStatus(status);
  return index;
}

media_status_t QueueInputBuffer(AMediaCodec* codec, size_t idx, abi::guest_off_t offset,
                                size_t size, uint64_t time, uint32_t flags) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (idx >= kCodecSlots || size > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    return AMEDIA_ERROR_INVALID_PARAMETER;
  }
  std::string error;
  int status = android::OK;
  if (!codec->engine->QueueInput(static_cast<int>(idx), offset, static_cast<int64_t>(size),
                                 static_cast<int64_t>(time), flags, &error, &status)) {
    return ToMediaStatus(status);
  }
  return AMEDIA_OK;
}

media_status_t QueueSecureInputBuffer(AMediaCodec* codec, size_t, abi::guest_off_t,
                                      AMediaCodecCryptoInfo*, uint64_t, uint32_t) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  return AMEDIA_ERROR_UNSUPPORTED;
}

abi::guest_ssize_t DequeueOutputBuffer(AMediaCodec* codec, abi::BufferInfo* info,
                                       int64_t timeout_us) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (info == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  CodecBufferInfo cinfo;
  int status = android::OK;
  const int index = codec->engine->DequeueOutput(&cinfo, timeout_us, &status);
  if (status != android::OK) return ToMediaStatus(status);
  if (index >= 0) {
    info->offset = cinfo.offset;
    info->size = cinfo.size;
    info->presentationTimeUs = cinfo.time_us;
    info->flags = cinfo.flags;
  }
  return index;
}

AMediaFormat* GetOutputFormat(AMediaCodec* codec) {
  if (codec == nullptr || codec->engine == nullptr) return nullptr;
  Format f;
  const bool available = codec->engine->IsConfigured() && codec->engine->GetOutputFormat(&f);
  return OwnedFormat(&f, available);
}

AMediaFormat* GetInputFormat(AMediaCodec* codec) {
  if (codec == nullptr || codec->engine == nullptr) return nullptr;
  Format f;
  const bool available = codec->engine->IsConfigured() && codec->engine->GetInputFormat(&f);
  return OwnedFormat(&f, available);
}

AMediaFormat* GetBufferFormat(AMediaCodec* codec, size_t idx) {
  if (codec == nullptr || codec->engine == nullptr) return nullptr;
  Format format;
  const bool available =
      idx < kCodecSlots && codec->engine->GetBufferFormat(static_cast<int>(idx), &format);
  return OwnedFormat(&format, available);
}

media_status_t ReleaseOutputBuffer(AMediaCodec* codec, size_t idx, bool render) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (idx >= kCodecSlots) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::string error;
  int status = android::OK;
  if (!codec->engine->ReleaseOutput(static_cast<int>(idx), render, false, 0, &error, &status)) {
    return ToMediaStatus(status);
  }
  return AMEDIA_OK;
}

media_status_t ReleaseOutputBufferAtTime(AMediaCodec* codec, size_t idx, int64_t timestamp_ns) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (idx >= kCodecSlots) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::string error;
  int status = android::OK;
  if (!codec->engine->ReleaseOutput(static_cast<int>(idx), true, true, timestamp_ns, &error,
                                    &status)) {
    return ToMediaStatus(status);
  }
  return AMEDIA_OK;
}

media_status_t SetOutputSurface(AMediaCodec* codec, ANativeWindow* surface) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (const QueueSurface* queue = surface != nullptr ? QueueSurface::From(surface) : nullptr) {
    // An abandoned consumer cannot take over from the current output; Android
    // fails to connect to it and reports AMEDIA_ERROR_UNKNOWN.
    int valid = 1;
    if (queue->query(NATIVE_WINDOW_IS_VALID, &valid) == 0 && valid == 0) {
      return AMEDIA_ERROR_UNKNOWN;
    }
  }
  int status = android::OK;
  codec->engine->SetOutputSurface(surface, &status);
  return ToMediaStatus(status);
}

media_status_t CreateInputSurface(AMediaCodec* codec, ANativeWindow** surface) {
  if (codec == nullptr || codec->engine == nullptr || surface == nullptr) {
    return AMEDIA_ERROR_INVALID_PARAMETER;
  }
  *surface = nullptr;
  std::lock_guard<std::mutex> lifecycle(codec->engine->lifecycle_mutex());
  int width = 0, height = 0;
  std::string error;
  if (!codec->engine->PrepareInputSurface(&width, &height, &error)) {
    return AMEDIA_ERROR_UNKNOWN;
  }
  // As MediaCodec::createInputSurface does, each call creates a new producer
  // and the encoder retires the previous one.
  android::sp<QueueSurface> qs(new QueueSurface(width, height));
  auto state = std::make_shared<NdkInputState>();
  state->owner = state->next_owner++;
  auto source = std::make_unique<NdkInputSource>(qs, false, state, state->owner);
  ANativeWindow* window = source->NativeWindow();
  ANativeWindow_acquire(window);
  codec->engine->AttachInputSource(std::move(source));
  *surface = window;
  return AMEDIA_OK;
}

media_status_t CreatePersistentInputSurface(ANativeWindow** surface) {
  if (surface == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  android::sp<QueueSurface> qs(new NdkPersistentSurface);
  ANativeWindow* window = qs.get();
  ANativeWindow_acquire(window);
  *surface = window;
  return AMEDIA_OK;
}

media_status_t SetInputSurface(AMediaCodec* codec, ANativeWindow* surface) {
  if (codec == nullptr || codec->engine == nullptr || surface == nullptr) {
    return AMEDIA_ERROR_INVALID_PARAMETER;
  }
  QueueSurface* qs = QueueSurface::From(surface);
  if (qs == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::shared_ptr<NdkInputState> state;
  {
    std::lock_guard<std::mutex> lock(gPersistentInputsMu);
    const auto found = gPersistentInputs.find(surface);
    if (found != gPersistentInputs.end()) state = found->second.lock();
  }
  if (!state) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lifecycle(codec->engine->lifecycle_mutex());
  int width = 0, height = 0;
  std::string error;
  // MediaCodec's state errors, including a producer another encoder still
  // consumes, reach the NDK caller as AMEDIA_ERROR_UNKNOWN.
  if (!codec->engine->PrepareInputSurface(&width, &height, &error)) {
    return AMEDIA_ERROR_UNKNOWN;
  }
  if (codec->engine->InputWindow() == surface) return AMEDIA_OK;
  uint64_t owner;
  {
    std::lock_guard<std::recursive_mutex> lock(state->mu);
    if (state->owner != 0 || state->next_owner == 0) return AMEDIA_ERROR_UNKNOWN;
    owner = state->owner = state->next_owner++;
    qs->SetDefaultSize(width, height);
  }
  auto source = std::make_unique<NdkInputSource>(android::sp<QueueSurface>(qs), true, state, owner);
  codec->engine->AttachInputSource(std::move(source));
  return AMEDIA_OK;
}

media_status_t SignalEndOfInputStream(AMediaCodec* codec) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::string error;
  int status = android::OK;
  codec->engine->SignalEndOfInputStream(&error, &status, /*reject_repeat=*/true);
  // NdkMediaCodec returns INVALID_OPERATION itself for this call.
  if (status == android::INVALID_OPERATION) return AMEDIA_ERROR_INVALID_OPERATION;
  return ToMediaStatus(status);
}

media_status_t SetParameters(AMediaCodec* codec, const AMediaFormat* params) {
  if (codec == nullptr || codec->engine == nullptr || params == nullptr) {
    return AMEDIA_ERROR_INVALID_PARAMETER;
  }
  Format copy;
  if (AMediaFormat_copy(copy.get(), const_cast<AMediaFormat*>(params)) != AMEDIA_OK) {
    return AMEDIA_ERROR_UNKNOWN;
  }
  const int status = codec->engine->SubmitLiveControls(copy);
  if (status == android::INVALID_OPERATION) return AMEDIA_ERROR_INVALID_OPERATION;
  return ToMediaStatus(status);
}

media_status_t GetName(AMediaCodec* codec, char** out_name) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (out_name == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  const std::string& name = codec->engine->entry().name;
  char* copy = static_cast<char*>(std::malloc(name.size() + 1));
  if (copy == nullptr) return AMEDIA_ERROR_UNKNOWN;
  std::memcpy(copy, name.c_str(), name.size() + 1);
  *out_name = copy;
  return AMEDIA_OK;
}

void ReleaseName(AMediaCodec*, char* name) { std::free(name); }

media_status_t SetAsyncNotifyCallback(AMediaCodec* codec, abi::OnAsyncNotifyCallback callback,
                                      void* userdata) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (!ResolveGuestCallback(&callback.onAsyncInputAvailable) ||
      !ResolveGuestCallback(&callback.onAsyncOutputAvailable) ||
      !ResolveGuestCallback(&callback.onAsyncFormatChanged) ||
      !ResolveGuestCallback(&callback.onAsyncError)) {
    return AMEDIA_ERROR_UNKNOWN;
  }
  const auto engine = codec->engine;
  const bool enable = callback.onAsyncInputAvailable != nullptr ||
                      callback.onAsyncOutputAvailable != nullptr ||
                      callback.onAsyncFormatChanged != nullptr ||
                      callback.onAsyncError != nullptr;
  std::shared_ptr<NativeCodecNotifications> state;
  if (enable) {
    state = std::make_shared<NativeCodecNotifications>();
    state->codec = codec;
    state->engine = engine;
    state->callback = callback;
    state->userdata = userdata;
  }
  // MediaCodec::setCallback's state error reaches the NDK caller as UNKNOWN.
  if (!enable) {
    if (!engine->SetAsync(false)) return AMEDIA_ERROR_UNKNOWN;
    std::shared_ptr<NativeCodecNotifications> retired;
    {
      std::lock_guard<std::mutex> lock(codec->async_mu);
      retired = std::move(codec->notifications);
    }
    if (retired) retired->queue->Close();
    return AMEDIA_OK;
  }
  CodecCallbacks cbs;
  cbs.on_input_available = [state](uint64_t generation, int index) {
    state->queue->Post([state, generation, index] {
      if (!state->IsCurrent(generation)) return;
      if (auto fn = state->callback.onAsyncInputAvailable) fn(state->codec, state->userdata, index);
    });
  };
  cbs.on_output_available = [state](uint64_t generation, int index, const CodecBufferInfo& info) {
    state->queue->Post([state, generation, index, info] {
      if (!state->IsCurrent(generation)) return;
      abi::BufferInfo binfo{info.offset, info.size, info.time_us, info.flags};
      if (auto fn = state->callback.onAsyncOutputAvailable) {
        fn(state->codec, state->userdata, index, &binfo);
      }
    });
  };
  cbs.on_output_format_changed = [state](uint64_t generation, const Format& format) {
    if (!state->callback.onAsyncFormatChanged) return;
    auto copy = std::make_shared<Format>();
    if (format.get() != nullptr) AMediaFormat_copy(copy->get(), format.get());
    state->queue->Post([state, generation, copy] {
      if (!state->IsCurrent(generation)) return;
      // As in NdkMediaCodec.cpp, ownership belongs to the NDK callback. A
      // cancelled notification retains its Format owner and frees it here.
      state->callback.onAsyncFormatChanged(state->codec, state->userdata, copy->Release());
    });
  };
  cbs.on_error = [state](uint64_t generation, int error, int action, const std::string& msg) {
    state->queue->Post([state, generation, error, action, msg] {
      if (!state->IsCurrent(generation)) return;
      if (auto fn = state->callback.onAsyncError) {
        fn(state->codec, state->userdata, ToMediaStatus(error), action, msg.c_str());
      }
    });
  };
  if (!engine->SetAsync(true, std::move(cbs))) {
    state->queue->Close();
    return AMEDIA_ERROR_UNKNOWN;
  }
  std::shared_ptr<NativeCodecNotifications> retired;
  {
    std::lock_guard<std::mutex> lock(codec->async_mu);
    retired = std::exchange(codec->notifications, std::move(state));
  }
  if (retired) retired->queue->Close();
  return AMEDIA_OK;
}

media_status_t SetOnFrameRenderedCallback(AMediaCodec* codec, abi::OnFrameRendered callback,
                                          void* userdata) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (!ResolveGuestCallback(&callback)) return AMEDIA_ERROR_UNKNOWN;
  std::shared_ptr<NativeFrameRendered> state;
  if (callback != nullptr) {
    state = std::make_shared<NativeFrameRendered>();
    state->codec = codec;
    state->callback = callback;
    state->userdata = userdata;
    std::weak_ptr<NativeFrameRendered> weak = state;
    codec->engine->EnableFrameRendered(true, [weak](int64_t media_time_us, int64_t system_nanos) {
      if (auto current = weak.lock()) current->Post(media_time_us, system_nanos);
    });
  } else {
    codec->engine->EnableFrameRendered(false);
  }
  std::shared_ptr<NativeFrameRendered> retired;
  {
    std::lock_guard<std::mutex> lock(codec->async_mu);
    retired = std::exchange(codec->frame_rendered, std::move(state));
  }
  // Close waits for a callback in flight on another thread, so no sample from
  // the replaced registration is delivered after this returns.
  if (retired) retired->queue->Close();
  return AMEDIA_OK;
}

// AAPCS64 passes this 32-byte by-value aggregate through a pointer, whereas
// x86-64 System V passes it on the stack. The translator selects this entry.
media_status_t SetAsyncNotifyCallbackIndirect(AMediaCodec* codec,
                                              const abi::OnAsyncNotifyCallback* callback,
                                              void* userdata) {
  if (callback == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  return SetAsyncNotifyCallback(codec, *callback, userdata);
}

media_status_t ReleaseCrypto(AMediaCodec* codec) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  return AMEDIA_OK;
}

bool ActionCodeIsRecoverable(int32_t action_code) {
  return action_code == abi::kActionCodeRecoverable;
}

bool ActionCodeIsTransient(int32_t action_code) {
  return action_code == abi::kActionCodeTransient;
}

void Register() {
  using gesher::bionic::AddBionicSymbol;
#define CODEC_ENTRY(name, fn) AddBionicSymbol(#name, reinterpret_cast<void*>(&Sysv<fn>::Call))
  CODEC_ENTRY(AMediaCodec_createCodecByName, CreateCodecByName);
  CODEC_ENTRY(AMediaCodec_createDecoderByType, CreateDecoderByType);
  CODEC_ENTRY(AMediaCodec_createEncoderByType, CreateEncoderByType);
  CODEC_ENTRY(AMediaCodec_delete, Delete);
  CODEC_ENTRY(AMediaCodec_configure, Configure);
  CODEC_ENTRY(AMediaCodec_start, Start);
  CODEC_ENTRY(AMediaCodec_stop, Stop);
  CODEC_ENTRY(AMediaCodec_flush, Flush);
  CODEC_ENTRY(AMediaCodec_getInputBuffer, GetInputBuffer);
  CODEC_ENTRY(AMediaCodec_getOutputBuffer, GetOutputBuffer);
  CODEC_ENTRY(AMediaCodec_dequeueInputBuffer, DequeueInputBuffer);
  CODEC_ENTRY(AMediaCodec_queueInputBuffer, QueueInputBuffer);
  CODEC_ENTRY(AMediaCodec_queueSecureInputBuffer, QueueSecureInputBuffer);
  CODEC_ENTRY(AMediaCodec_dequeueOutputBuffer, DequeueOutputBuffer);
  CODEC_ENTRY(AMediaCodec_getOutputFormat, GetOutputFormat);
  CODEC_ENTRY(AMediaCodec_getInputFormat, GetInputFormat);
  CODEC_ENTRY(AMediaCodec_getBufferFormat, GetBufferFormat);
  CODEC_ENTRY(AMediaCodec_releaseOutputBuffer, ReleaseOutputBuffer);
  CODEC_ENTRY(AMediaCodec_releaseOutputBufferAtTime, ReleaseOutputBufferAtTime);
  CODEC_ENTRY(AMediaCodec_setOutputSurface, SetOutputSurface);
  CODEC_ENTRY(AMediaCodec_setOnFrameRenderedCallback, SetOnFrameRenderedCallback);
  CODEC_ENTRY(AMediaCodec_createInputSurface, CreateInputSurface);
  CODEC_ENTRY(AMediaCodec_createPersistentInputSurface, CreatePersistentInputSurface);
  CODEC_ENTRY(AMediaCodec_setInputSurface, SetInputSurface);
  CODEC_ENTRY(AMediaCodec_signalEndOfInputStream, SignalEndOfInputStream);
  CODEC_ENTRY(AMediaCodec_setParameters, SetParameters);
  CODEC_ENTRY(AMediaCodec_getName, GetName);
  CODEC_ENTRY(AMediaCodec_releaseName, ReleaseName);
  CODEC_ENTRY(AMediaCodec_setAsyncNotifyCallback, SetAsyncNotifyCallback);
  CODEC_ENTRY(__gesher_AMediaCodec_setAsyncNotifyCallback, SetAsyncNotifyCallbackIndirect);
  CODEC_ENTRY(AMediaCodec_releaseCrypto, ReleaseCrypto);
  CODEC_ENTRY(AMediaCodecActionCode_isRecoverable, ActionCodeIsRecoverable);
  CODEC_ENTRY(AMediaCodecActionCode_isTransient, ActionCodeIsTransient);
#undef CODEC_ENTRY
  // Publish each pointer object's address for dlsym and ELF data relocations.
#define CODEC_KEY(name) AddBionicSymbol(#name, static_cast<void*>(&name))
  CODEC_KEY(AMEDIACODEC_KEY_HDR10_PLUS_INFO);
  CODEC_KEY(AMEDIACODEC_KEY_LOW_LATENCY);
  CODEC_KEY(AMEDIACODEC_KEY_OFFSET_TIME);
  CODEC_KEY(AMEDIACODEC_KEY_REQUEST_SYNC_FRAME);
  CODEC_KEY(AMEDIACODEC_KEY_SUSPEND);
  CODEC_KEY(AMEDIACODEC_KEY_SUSPEND_TIME);
  CODEC_KEY(AMEDIACODEC_KEY_VIDEO_BITRATE);
#undef CODEC_KEY
}

[[maybe_unused]] const bool kRegistered = (Register(), true);

}  // namespace
}  // namespace gesher::media
