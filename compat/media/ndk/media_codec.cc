// The NDK's MediaCodec (libmediandk.so) for loaded ELF code, backed by
// Gesher's shared CodecEngine (compat/media/codec/codec_engine.h).

#include "media_codec_abi.h"

#include <gesher/bionic_shim.h>
#include <media/NdkMediaErrorPriv.h>
#include <media/NdkMediaFormat.h>
#include <utils/Errors.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>

#include "../codec/codec_engine.h"
#include "../surface/queue_surface.h"

extern "C" {
void ANativeWindow_acquire(ANativeWindow* window);
void ANativeWindow_release(ANativeWindow* window);
}

struct AMediaCodec {
  std::shared_ptr<gesher::media::CodecEngine> engine;
  std::mutex async_mu;
  gesher::media::codec_abi::OnAsyncNotifyCallback callback{};
  void* userdata = nullptr;
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

class NdkInputSource final : public InputFrameSource {
 public:
  NdkInputSource(android::sp<QueueSurface> surface, bool persistent)
      : surface_(std::move(surface)), persistent_(persistent) {}
  ~NdkInputSource() override { Abandon(); }

  void SetCallback(FrameCallback cb) override {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->abandoned) return;
    state_->cb = std::move(cb);
    QueueSurface* raw = surface_.get();
    std::shared_ptr<State> s = state_;
    surface_->SetListener([raw, s] {
      FrameCallback fn;
      {
        std::lock_guard<std::mutex> lk(s->mu);
        if (s->abandoned || !s->cb) return;
        fn = s->cb;
      }
      std::shared_ptr<const SurfaceFrame> f = raw->Latest();
      if (f != nullptr && !f->pixels.empty()) {
        fn(f->pixels.data(), f->width, f->height, f->timestamp_ns);
      }
    });
  }

  void Abandon() override {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->abandoned) return;
    state_->abandoned = true;
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
  struct State {
    std::mutex mu;
    bool abandoned = false;
    FrameCallback cb;
  };
  std::shared_ptr<State> state_ = std::make_shared<State>();
  android::sp<QueueSurface> surface_;
  const bool persistent_;
};

media_status_t ToMediaStatus(int status) {
  if (status == android::OK) return AMEDIA_OK;
  if (status == android::INVALID_OPERATION) return AMEDIA_ERROR_INVALID_OPERATION;
  if (status == android::BAD_VALUE) return AMEDIA_ERROR_INVALID_PARAMETER;
  return translate_error(status);
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
  if (codec->engine != nullptr) codec->engine->Release();
  delete codec;
  return AMEDIA_OK;
}

media_status_t Configure(AMediaCodec* codec, const AMediaFormat* format, ANativeWindow* surface,
                         AMediaCrypto* crypto, uint32_t flags) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (format == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  if (crypto != nullptr) return AMEDIA_ERROR_UNSUPPORTED;
  const bool encode_flag = (flags & abi::kConfigureFlagEncode) != 0;
  if (encode_flag != codec->engine->entry().encoder) return AMEDIA_ERROR_INVALID_PARAMETER;

  Format copy;
  if (AMediaFormat_copy(copy.get(), const_cast<AMediaFormat*>(format)) != AMEDIA_OK) {
    return AMEDIA_ERROR_UNKNOWN;
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
  std::string error;
  return codec->engine->Start(&error) ? AMEDIA_OK : AMEDIA_ERROR_INVALID_OPERATION;
}

media_status_t Stop(AMediaCodec* codec) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  int status = android::OK;
  codec->engine->Stop(&status);
  return ToMediaStatus(status);
}

media_status_t Flush(AMediaCodec* codec) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  int status = android::OK;
  codec->engine->Flush(&status);
  return ToMediaStatus(status);
}

uint8_t* GetInputBuffer(AMediaCodec* codec, size_t idx, size_t* out_size) {
  if (out_size != nullptr) *out_size = 0;
  if (codec == nullptr || codec->engine == nullptr) return nullptr;
  return codec->engine->GetInputBuffer(static_cast<int>(idx), out_size);
}

uint8_t* GetOutputBuffer(AMediaCodec* codec, size_t idx, size_t* out_size) {
  if (out_size != nullptr) *out_size = 0;
  if (codec == nullptr || codec->engine == nullptr) return nullptr;
  return codec->engine->GetOutputBuffer(static_cast<int>(idx), out_size);
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
  if (!codec->engine->GetOutputFormat(&f)) return nullptr;
  return f.Release();
}

AMediaFormat* GetInputFormat(AMediaCodec* codec) {
  if (codec == nullptr || codec->engine == nullptr) return nullptr;
  Format f;
  if (!codec->engine->GetInputFormat(&f)) return nullptr;
  return f.Release();
}

AMediaFormat* GetBufferFormat(AMediaCodec* codec, size_t) {
  return GetOutputFormat(codec);
}

media_status_t ReleaseOutputBuffer(AMediaCodec* codec, size_t idx, bool render) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  std::string error;
  int status = android::OK;
  if (!codec->engine->ReleaseOutput(static_cast<int>(idx), render, false, 0, &error, &status)) {
    return ToMediaStatus(status);
  }
  return AMEDIA_OK;
}

media_status_t ReleaseOutputBufferAtTime(AMediaCodec* codec, size_t idx, int64_t timestamp_ns) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
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
  int status = android::OK;
  codec->engine->SetSurface(surface, nullptr, &status);
  return ToMediaStatus(status);
}

media_status_t CreateInputSurface(AMediaCodec* codec, ANativeWindow** surface) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (surface == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  *surface = nullptr;
  std::lock_guard<std::mutex> lifecycle(codec->engine->lifecycle_mutex());
  int width = 0, height = 0;
  std::string error;
  if (!codec->engine->PrepareInputSurface(&width, &height, &error)) {
    return AMEDIA_ERROR_INVALID_OPERATION;
  }
  android::sp<QueueSurface> qs(new QueueSurface(width, height));
  auto source = std::make_unique<NdkInputSource>(qs, false);
  ANativeWindow* window = source->NativeWindow();
  ANativeWindow_acquire(window);
  codec->engine->AttachInputSource(std::move(source));
  *surface = window;
  return AMEDIA_OK;
}

media_status_t CreatePersistentInputSurface(ANativeWindow** surface) {
  if (surface == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  android::sp<QueueSurface> qs(new QueueSurface(1, 1));
  ANativeWindow* window = qs.get();
  ANativeWindow_acquire(window);
  *surface = window;
  return AMEDIA_OK;
}

media_status_t SetInputSurface(AMediaCodec* codec, ANativeWindow* surface) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (surface == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  QueueSurface* qs = QueueSurface::From(surface);
  if (qs == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lifecycle(codec->engine->lifecycle_mutex());
  int width = 0, height = 0;
  std::string error;
  if (!codec->engine->PrepareInputSurface(&width, &height, &error)) {
    return AMEDIA_ERROR_INVALID_OPERATION;
  }
  qs->SetDefaultSize(width, height);
  auto source = std::make_unique<NdkInputSource>(android::sp<QueueSurface>(qs), true);
  codec->engine->AttachInputSource(std::move(source));
  return AMEDIA_OK;
}

media_status_t SignalEndOfInputStream(AMediaCodec* codec) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  std::string error;
  return codec->engine->SignalEndOfInputStream(&error) ? AMEDIA_OK
                                                       : AMEDIA_ERROR_INVALID_OPERATION;
}

media_status_t SetParameters(AMediaCodec* codec, const AMediaFormat* params) {
  if (codec == nullptr || codec->engine == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (params == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  Format copy;
  if (AMediaFormat_copy(copy.get(), const_cast<AMediaFormat*>(params)) != AMEDIA_OK) {
    return AMEDIA_ERROR_UNKNOWN;
  }
  return codec->engine->SetParameters(copy) ? AMEDIA_OK : AMEDIA_ERROR_INVALID_OPERATION;
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
  const bool enable = callback.onAsyncInputAvailable != nullptr ||
                      callback.onAsyncOutputAvailable != nullptr ||
                      callback.onAsyncFormatChanged != nullptr ||
                      callback.onAsyncError != nullptr;
  {
    std::lock_guard<std::mutex> lock(codec->async_mu);
    codec->callback = callback;
    codec->userdata = userdata;
  }
  if (!enable) {
    codec->engine->SetAsync(false);
    return AMEDIA_OK;
  }
  CodecCallbacks cbs;
  cbs.on_input_available = [codec](int index) {
    abi::OnAsyncInputAvailable fn = nullptr;
    void* ud = nullptr;
    {
      std::lock_guard<std::mutex> lock(codec->async_mu);
      fn = codec->callback.onAsyncInputAvailable;
      ud = codec->userdata;
    }
    if (fn != nullptr) fn(codec, ud, index);
  };
  cbs.on_output_available = [codec](int index, const CodecBufferInfo& info) {
    abi::OnAsyncOutputAvailable fn = nullptr;
    void* ud = nullptr;
    {
      std::lock_guard<std::mutex> lock(codec->async_mu);
      fn = codec->callback.onAsyncOutputAvailable;
      ud = codec->userdata;
    }
    if (fn != nullptr) {
      abi::BufferInfo binfo{info.offset, info.size, info.time_us, info.flags};
      fn(codec, ud, index, &binfo);
    }
  };
  cbs.on_output_format_changed = [codec](const Format& format) {
    abi::OnAsyncFormatChanged fn = nullptr;
    void* ud = nullptr;
    {
      std::lock_guard<std::mutex> lock(codec->async_mu);
      fn = codec->callback.onAsyncFormatChanged;
      ud = codec->userdata;
    }
    if (fn != nullptr) {
      Format copy;
      if (format.get() != nullptr) AMediaFormat_copy(copy.get(), format.get());
      fn(codec, ud, copy.get());
    }
  };
  cbs.on_error = [codec](int error, int action, const std::string& msg) {
    abi::OnAsyncError fn = nullptr;
    void* ud = nullptr;
    {
      std::lock_guard<std::mutex> lock(codec->async_mu);
      fn = codec->callback.onAsyncError;
      ud = codec->userdata;
    }
    if (fn != nullptr) {
      fn(codec, ud, ToMediaStatus(error), action, msg.c_str());
    }
  };
  codec->engine->SetAsync(true, std::move(cbs));
  return AMEDIA_OK;
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
  CODEC_ENTRY(AMediaCodec_createInputSurface, CreateInputSurface);
  CODEC_ENTRY(AMediaCodec_createPersistentInputSurface, CreatePersistentInputSurface);
  CODEC_ENTRY(AMediaCodec_setInputSurface, SetInputSurface);
  CODEC_ENTRY(AMediaCodec_signalEndOfInputStream, SignalEndOfInputStream);
  CODEC_ENTRY(AMediaCodec_setParameters, SetParameters);
  CODEC_ENTRY(AMediaCodec_getName, GetName);
  CODEC_ENTRY(AMediaCodec_releaseName, ReleaseName);
  CODEC_ENTRY(AMediaCodec_setAsyncNotifyCallback, SetAsyncNotifyCallback);
  CODEC_ENTRY(AMediaCodec_releaseCrypto, ReleaseCrypto);
  CODEC_ENTRY(AMediaCodecActionCode_isRecoverable, ActionCodeIsRecoverable);
  CODEC_ENTRY(AMediaCodecActionCode_isTransient, ActionCodeIsTransient);
#undef CODEC_ENTRY
}

[[maybe_unused]] const bool kRegistered = (Register(), true);

}  // namespace
}  // namespace gesher::media
