// Gesher: android.media.MediaCodec and MediaCodecList over Gesher's shared
// CodecEngine (compat/media/codec/codec_engine.h), in libmedia_jni.so.

#include <jni.h>
#include <gesher/clocks.h>
#include <media/NdkMediaFormat.h>
#include <utils/Errors.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../codec/codec_engine.h"
#include "../video/input_surface.h"
#include "../../devices/camera_frames.h"
#include "jni_util.h"
#include "media_jni.h"
#include "persistent_input_surface.h"

struct ANativeWindow;
extern "C" {
ANativeWindow* ANativeWindow_fromSurface(JNIEnv* env, jobject surface);
void ANativeWindow_release(ANativeWindow* window);
}

namespace gesher::media {
namespace {

constexpr int kInfoTryAgainLater = kCodecInfoTryAgainLater;
constexpr int kEventCallback = 1;
constexpr int kEventFrameRendered = 3;
constexpr int kCbInputAvailable = 1;
constexpr int kCbOutputAvailable = 2;
constexpr int kCbError = 3;
constexpr int kCbOutputFormatChange = 4;

// --- Java helpers ---------------------------------------------------------------------------

struct {
  jfieldID context;
  jfieldID listener_lock;
  jmethodID post_event;
  jclass buffer_info;
  jmethodID buffer_info_ctor;
  jmethodID buffer_info_set;
  jclass codec_exception;
  jmethodID codec_exception_ctor;
  jclass media_image;
  jmethodID media_image_ctor;
  jclass rect;
  jmethodID rect_ctor;
  jclass byte_buffer;
  jmethodID as_read_only, order, position, limit, clear;
  jobject native_order;
  jclass hash_map;
  jmethodID hash_map_ctor, hash_map_put;
  jclass persistable_bundle;
  jmethodID persistable_bundle_ctor, bundle_put_int, bundle_put_string;
} gJ;

jobject MakeMetricsBundle(JNIEnv* env, const CodecEntry& entry,
                          const std::optional<CodecMetrics>& metrics) {
  jobject bundle = env->NewObject(gJ.persistable_bundle, gJ.persistable_bundle_ctor);
  if (bundle == nullptr) return nullptr;
  const std::string prefix = "android.media.mediacodec.";
  auto put_int = [&](const char* name, jint value) {
    if (env->ExceptionCheck()) return;
    jstring key = env->NewStringUTF((prefix + name).c_str());
    if (key == nullptr) return;
    env->CallVoidMethod(bundle, gJ.bundle_put_int, key, value);
    env->DeleteLocalRef(key);
  };
  auto put_string = [&](const char* name, const char* value) {
    if (env->ExceptionCheck()) return;
    jstring key = env->NewStringUTF((prefix + name).c_str());
    if (key == nullptr) return;
    jstring string_value = env->NewStringUTF(value);
    if (string_value != nullptr) env->CallVoidMethod(bundle, gJ.bundle_put_string, key, string_value);
    env->DeleteLocalRef(string_value);
    env->DeleteLocalRef(key);
  };
  put_string("codec", entry.name.c_str());
  put_string("mode", entry.video ? "video" : "audio");
  put_int("secure", 0);
  put_int("hardware", entry.hardware ? 1 : 0);
  if (!metrics) return bundle;
  put_string("mime", entry.mime.c_str());
  put_int("encoder", entry.encoder ? 1 : 0);
  if (entry.video) {
    if (metrics->width > 0) put_int("width", metrics->width);
    if (metrics->height > 0) put_int("height", metrics->height);
    put_int("rotation", metrics->rotation);
    put_int("rotation-degrees", metrics->rotation);
  } else {
    if (metrics->sample_rate > 0) put_int("sampleRate", metrics->sample_rate);
    if (metrics->channel_count > 0) put_int("channelCount", metrics->channel_count);
  }
  if (metrics->bitrate > 0) put_int("bitrate", metrics->bitrate);
  return bundle;
}

jobject Box(JNIEnv* env, int64_t v) {
  jclass c = env->FindClass("java/lang/Long");
  return env->CallStaticObjectMethod(
      c, env->GetStaticMethodID(c, "valueOf", "(J)Ljava/lang/Long;"), static_cast<jlong>(v));
}

struct JavaMap {
  std::map<std::string, int64_t> longs;

  jobject Make(JNIEnv* env) const {
    jobject map = env->NewObject(gJ.hash_map, gJ.hash_map_ctor);
    auto put = [&](const std::string& k, jobject v) {
      jstring key = env->NewStringUTF(k.c_str());
      env->DeleteLocalRef(env->CallObjectMethod(map, gJ.hash_map_put, key, v));
      env->DeleteLocalRef(key);
      env->DeleteLocalRef(v);
    };
    for (const auto& [k, v] : longs) put(k, Box(env, v));
    return map;
  }
};

jobject NewByteBuffer(JNIEnv* env, uint8_t* p, size_t capacity, bool read_only) {
  jobject b = env->NewDirectByteBuffer(p, static_cast<jlong>(capacity));
  if (b == nullptr) return nullptr;
  jobject ordered = env->CallObjectMethod(b, gJ.order, gJ.native_order);
  env->DeleteLocalRef(ordered);
  if (read_only) {
    jobject ro = env->CallObjectMethod(b, gJ.as_read_only);
    env->DeleteLocalRef(b);
    b = ro;
    ordered = env->CallObjectMethod(b, gJ.order, gJ.native_order);
    env->DeleteLocalRef(ordered);
  }
  return b;
}

void SetRange(JNIEnv* env, jobject b, int offset, int size) {
  env->DeleteLocalRef(env->CallObjectMethod(b, gJ.clear));
  env->DeleteLocalRef(env->CallObjectMethod(b, gJ.limit, offset + size));
  env->DeleteLocalRef(env->CallObjectMethod(b, gJ.position, offset));
}

void ThrowCodecException(JNIEnv* env, int error, const std::string& message) {
  jstring m = env->NewStringUTF(message.c_str());
  auto e = static_cast<jthrowable>(
      env->NewObject(gJ.codec_exception, gJ.codec_exception_ctor, error, 0, m));
  env->DeleteLocalRef(m);
  if (e != nullptr) env->Throw(e);
}

void ThrowCodecStatus(JNIEnv* env, int status, const std::string& message) {
  if (status == android::OK) return;
  if (status == android::INVALID_OPERATION) {
    jniThrow(env, "java/lang/IllegalStateException", message.c_str());
  } else if (status == android::BAD_VALUE) {
    jniThrow(env, "java/lang/IllegalArgumentException", message.c_str());
  } else {
    ThrowCodecException(env, status, message);
  }
}

Format FormatFromJava(JNIEnv* env, jobjectArray keys, jobjectArray values) {
  Format f;
  if (keys == nullptr || values == nullptr) return f;
  jclass integer = env->FindClass("java/lang/Integer");
  jclass long_c = env->FindClass("java/lang/Long");
  jclass float_c = env->FindClass("java/lang/Float");
  jclass string_c = env->FindClass("java/lang/String");
  jmethodID int_value = env->GetMethodID(integer, "intValue", "()I");
  jmethodID long_value = env->GetMethodID(long_c, "longValue", "()J");
  jmethodID float_value = env->GetMethodID(float_c, "floatValue", "()F");
  jmethodID remaining = env->GetMethodID(gJ.byte_buffer, "remaining", "()I");
  jmethodID pos = env->GetMethodID(gJ.byte_buffer, "position", "()I");
  jmethodID get = env->GetMethodID(gJ.byte_buffer, "get", "(I)B");
  jsize n = std::min(env->GetArrayLength(keys), env->GetArrayLength(values));
  for (jsize i = 0; i < n; i++) {
    jstring k = static_cast<jstring>(env->GetObjectArrayElement(keys, i));
    jobject v = env->GetObjectArrayElement(values, i);
    if (k == nullptr || v == nullptr) continue;
    std::string key = Utf8(env, k);
    if (env->IsInstanceOf(v, integer)) {
      AMediaFormat_setInt32(f.get(), key.c_str(), env->CallIntMethod(v, int_value));
    } else if (env->IsInstanceOf(v, long_c)) {
      AMediaFormat_setInt64(f.get(), key.c_str(), env->CallLongMethod(v, long_value));
    } else if (env->IsInstanceOf(v, float_c)) {
      AMediaFormat_setFloat(f.get(), key.c_str(), env->CallFloatMethod(v, float_value));
    } else if (env->IsInstanceOf(v, string_c)) {
      AMediaFormat_setString(f.get(), key.c_str(), Utf8(env, static_cast<jstring>(v)).c_str());
    } else if (env->IsInstanceOf(v, gJ.byte_buffer)) {
      jint size = env->CallIntMethod(v, remaining);
      jint start = env->CallIntMethod(v, pos);
      std::vector<uint8_t> bytes(static_cast<size_t>(std::max(size, 0)));
      auto* direct = static_cast<uint8_t*>(env->GetDirectBufferAddress(v));
      if (direct != nullptr) {
        memcpy(bytes.data(), direct + start, bytes.size());
      } else {
        for (jint j = 0; j < size; j++) {
          bytes[static_cast<size_t>(j)] =
              static_cast<uint8_t>(env->CallByteMethod(v, get, start + j));
        }
      }
      AMediaFormat_setBuffer(f.get(), key.c_str(), bytes.data(), bytes.size());
    }
    env->DeleteLocalRef(k);
    env->DeleteLocalRef(v);
  }
  return f;
}

// --- ImageReader surfaces ------------------------------------------------------------------

jclass CameraFramesClass(JNIEnv* env) {
  static jclass cls = [env] {
    jclass local = env->FindClass("com/gesher/server/CameraFrames");
    if (local == nullptr) {
      env->ExceptionClear();
      return static_cast<jclass>(nullptr);
    }
    auto g = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return g;
  }();
  return cls;
}

bool IsReaderSurface(JNIEnv* env, jobject surface) {
  jclass cls = CameraFramesClass(env);
  if (cls == nullptr) return false;
  jmethodID m = env->GetStaticMethodID(cls, "isReader", "(Landroid/view/Surface;)Z");
  if (m == nullptr) {
    env->ExceptionClear();
    return false;
  }
  jboolean r = env->CallStaticBooleanMethod(cls, m, surface);
  if (CheckException(env, "CameraFrames.isReader")) return false;
  return r == JNI_TRUE;
}

int DeliverToReader(jobject surface, const VideoFrame& f, int64_t timestamp_ns) {
  JNIEnv* env = AttachedEnv("gesher-codec");
  if (env == nullptr) return android::UNKNOWN_ERROR;
  const int result = android::QueueImageReaderVideoFrame(env, surface, f, timestamp_ns);
  if (CheckException(env, "ImageReader native delivery")) return android::UNKNOWN_ERROR;
  return result == 1 ? android::BAD_VALUE : result;
}

class JniInputSource final : public InputFrameSource {
 public:
  explicit JniInputSource(std::shared_ptr<InputSurface> input) : input_(std::move(input)) {}
  void SetCallback(FrameCallback cb) override {
    if (input_ != nullptr) input_->SetCallback(std::move(cb));
  }
  void Abandon() override {
    if (input_ != nullptr) input_->Abandon();
  }
  ANativeWindow* NativeWindow() override { return nullptr; }

 private:
  std::shared_ptr<InputSurface> input_;
};

// --- JNI Codec adapter ----------------------------------------------------------------------

class Codec {
 public:
  Codec(JNIEnv* env, jobject thiz, const CodecEntry& entry)
      : engine_(std::make_shared<CodecEngine>(entry)),
        self_(std::make_shared<WeakRef>(env, thiz)) {}

  const CodecEntry& entry() const { return engine_->entry(); }

  void Release() {
    engine_->Release();
    std::lock_guard<std::mutex> lk(jni_mu_);
    java_input_.reset();
    persistent_input_.reset();
  }

  void Reset() {
    engine_->Reset();
    std::lock_guard<std::mutex> lk(jni_mu_);
    java_input_.reset();
    persistent_input_.reset();
  }

  void Stop(int* status) {
    engine_->Stop(status);
    if (status == nullptr || *status == android::OK) {
      std::lock_guard<std::mutex> lk(jni_mu_);
      java_input_.reset();
      persistent_input_.reset();
    }
  }

  bool Configure(JNIEnv* env, Format format, jobject surface, std::string* error, int* status) {
    ANativeWindow* window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    SurfaceDeliverHook hook = MakeReaderHook(env, surface, window);
    const bool ok = engine_->Configure(std::move(format), window, std::move(hook), error, status);
    if (window != nullptr) ANativeWindow_release(window);
    return ok;
  }

  void SetSurface(JNIEnv* env, jobject surface, int* status) {
    if (engine_->IsReleasedOrFatal()) {
      if (status) *status = android::INVALID_OPERATION;
      return;
    }
    ANativeWindow* window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    SurfaceDeliverHook hook = MakeReaderHook(env, surface, window);
    engine_->SetSurface(window, std::move(hook), status);
    if (window != nullptr) ANativeWindow_release(window);
  }

  jobject CreateInputSurface(JNIEnv* env, std::string* error) {
    std::lock_guard<std::mutex> lifecycle(engine_->lifecycle_mutex());
    int width = 0, height = 0;
    if (!engine_->PrepareInputSurface(&width, &height, error)) return nullptr;
    {
      std::lock_guard<std::mutex> lk(jni_mu_);
      if (java_input_ != nullptr && persistent_input_ == nullptr) {
        return java_input_->NewJavaSurface(env);
      }
    }
    std::shared_ptr<InputSurface> input(gesher::media::CreateInputSurface(width, height));
    if (input == nullptr) {
      *error = "cannot create the input Surface";
      return nullptr;
    }
    jobject surface = input->NewJavaSurface(env);
    if (surface == nullptr) {
      *error = "cannot create the Java input Surface";
      return nullptr;
    }
    auto source = std::make_unique<JniInputSource>(input);
    engine_->AttachInputSource(std::move(source), [&] {
      std::lock_guard<std::mutex> lk(jni_mu_);
      java_input_ = std::move(input);
      persistent_input_.reset();
    });
    return surface;
  }

  bool SetInputSurface(const std::shared_ptr<PersistentInputSurface>& source, std::string* error) {
    if (source == nullptr) {
      *error = "setInputSurface() requires a configured video encoder before start()";
      return false;
    }
    std::lock_guard<std::mutex> lifecycle(engine_->lifecycle_mutex());
    int width = 0, height = 0;
    if (!engine_->PrepareInputSurface(&width, &height, error)) return false;
    {
      std::lock_guard<std::mutex> lk(jni_mu_);
      if (source == persistent_input_) return true;
    }
    std::shared_ptr<InputSurface> input(source->Attach(width, height));
    if (input == nullptr) {
      *error = "persistent input Surface already has a consumer";
      return false;
    }
    auto wrapper = std::make_unique<JniInputSource>(input);
    engine_->AttachInputSource(std::move(wrapper), [&] {
      std::lock_guard<std::mutex> lk(jni_mu_);
      java_input_ = std::move(input);
      persistent_input_ = source;
    });
    return true;
  }

  bool SetAsync(bool async) {
    if (!async) {
      return engine_->SetAsync(false);
    }
    std::shared_ptr<WeakRef> self = self_;
    std::weak_ptr<CodecEngine> engine = engine_;
    CodecCallbacks cbs;
    cbs.on_input_available = [self, engine](uint64_t generation, int index) {
      PostEvent(engine, self, generation, kCbInputAvailable, index, nullptr);
    };
    cbs.on_output_available = [self, engine](uint64_t generation, int index, const CodecBufferInfo& info) {
      JNIEnv* env = AttachedEnv("gesher-codec");
      if (env == nullptr) return;
      env->PushLocalFrame(16);
      jobject jinfo = env->NewObject(gJ.buffer_info, gJ.buffer_info_ctor);
      env->CallVoidMethod(jinfo, gJ.buffer_info_set, info.offset, info.size,
                          static_cast<jlong>(info.time_us), static_cast<jint>(info.flags));
      PostEvent(engine, self, generation, kCbOutputAvailable, index, jinfo);
      env->PopLocalFrame(nullptr);
    };
    cbs.on_output_format_changed = [self, engine](uint64_t generation, const Format& format) {
      JNIEnv* env = AttachedEnv("gesher-codec");
      if (env == nullptr) return;
      env->PushLocalFrame(64);
      jobject map = FormatToMap(env, format);
      PostEvent(engine, self, generation, kCbOutputFormatChange, 0, map);
      env->PopLocalFrame(nullptr);
    };
    cbs.on_error = [self, engine](uint64_t generation, int error, int action, const std::string& msg) {
      JNIEnv* env = AttachedEnv("gesher-codec");
      if (env == nullptr) return;
      jstring m = env->NewStringUTF(msg.c_str());
      if (m == nullptr) {
        CheckException(env, "MediaCodec error message");
        return;
      }
      jobject e = env->NewObject(gJ.codec_exception, gJ.codec_exception_ctor, error, action, m);
      if (e != nullptr) PostEvent(engine, self, generation, kCbError, 0, e);
      CheckException(env, "MediaCodec error callback");
      env->DeleteLocalRef(e);
      env->DeleteLocalRef(m);
    };
    return engine_->SetAsync(true, std::move(cbs));
  }

  void EnableFrameRendered(bool enable) {
    std::shared_ptr<WeakRef> self = self_;
    engine_->EnableFrameRendered(enable, [self](int64_t time_us, int64_t system_ns) {
      JNIEnv* env = AttachedEnv("gesher-codec");
      if (env == nullptr) return;
      jobject s = env->NewLocalRef(self->get());
      if (s == nullptr) return;
      JavaMap m;
      m.longs["0-media-time-us"] = time_us;
      m.longs["0-system-nano"] = system_ns;
      jobject map = m.Make(env);
      env->CallVoidMethod(s, gJ.post_event, kEventFrameRendered, 0, 0, map);
      CheckException(env, "MediaCodec.postEventFromNative");
      env->DeleteLocalRef(map);
      env->DeleteLocalRef(s);
    });
  }

  bool Start(std::string* error) { return engine_->Start(error); }
  void Flush(int* status) { engine_->Flush(status); }
  int DequeueInput(int64_t timeout_us, int* status) { return engine_->DequeueInput(timeout_us, status); }
  bool QueueInput(int index, int offset, int size, int64_t time_us, int flags, std::string* error) {
    return engine_->QueueInput(index, offset, size, time_us, static_cast<uint32_t>(flags), error);
  }
  int DequeueOutput(JNIEnv* env, jobject info, int64_t timeout_us, int* status) {
    CodecBufferInfo cinfo;
    int index = engine_->DequeueOutput(&cinfo, timeout_us, status);
    if (index >= 0) {
      env->CallVoidMethod(info, gJ.buffer_info_set, cinfo.offset, cinfo.size,
                          static_cast<jlong>(cinfo.time_us), static_cast<jint>(cinfo.flags));
    }
    return index;
  }
  bool ReleaseOutput(int index, bool render, bool has_timestamp, int64_t time_ns,
                     std::string* error, int* status) {
    return engine_->ReleaseOutput(index, render, has_timestamp, time_ns, error, status);
  }
  bool SignalEndOfInputStream(std::string* error) { return engine_->SignalEndOfInputStream(error); }
  bool SetParameters(const Format& p) { return engine_->SetParameters(p); }
  bool SetVideoScalingMode(int mode) { return engine_->SetVideoScalingMode(mode); }

  jobject GetMetrics(JNIEnv* env) {
    std::optional<CodecMetrics> metrics;
    if (!engine_->GetMetrics(&metrics)) {
      jniThrow(env, "java/lang/IllegalStateException", "getMetrics() after release()");
      return nullptr;
    }
    return MakeMetricsBundle(env, engine_->entry(), metrics);
  }

  jobject InputBuffer(JNIEnv* env, int index) {
    size_t cap = 0;
    int status = android::OK;
    uint8_t* p = engine_->GetInputBuffer(index, &cap, &status);
    if (p == nullptr) {
      if (status == android::INVALID_OPERATION && engine_->IsReleasedOrFatal()) {
        jniThrow(env, "java/lang/IllegalStateException",
                 "codec is released or requires reset after a fatal error");
      }
      return nullptr;
    }
    return NewByteBuffer(env, p, cap, false);
  }

  jobject OutputBuffer(JNIEnv* env, int index) {
    size_t cap = 0;
    bool oom = false;
    int status = android::OK;
    int data_size = 0;
    uint8_t* p = engine_->GetOutputBuffer(index, &cap, &oom, &status, &data_size);
    if (oom) {
      jniThrow(env, "java/lang/OutOfMemoryError", "MediaCodec output frame");
      return nullptr;
    }
    if (p == nullptr) {
      if (status == android::INVALID_OPERATION && engine_->IsReleasedOrFatal()) {
        jniThrow(env, "java/lang/IllegalStateException",
                 "codec is released or requires reset after a fatal error");
      }
      return nullptr;
    }
    jobject b = NewByteBuffer(env, p, cap, true);
    if (b != nullptr) SetRange(env, b, 0, data_size);
    return b;
  }

  jobjectArray Buffers(JNIEnv* env, bool input) {
    jobjectArray a = env->NewObjectArray(kCodecSlots, gJ.byte_buffer, nullptr);
    for (int i = 0; i < kCodecSlots; i++) {
      jobject b = input ? InputBuffer(env, i) : OutputBuffer(env, i);
      if (env->ExceptionCheck()) {
        env->DeleteLocalRef(a);
        return nullptr;
      }
      if (b == nullptr) continue;
      env->SetObjectArrayElement(a, i, b);
      env->DeleteLocalRef(b);
    }
    return a;
  }

  jobject OutputImage(JNIEnv* env, int index, bool input) {
    uint8_t* data = nullptr;
    size_t size = 0;
    int w = 0, h = 0;
    int64_t t = 0;
    bool oom = false;
    int status = android::OK;
    if (!engine_->GetVideoImageSlot(index, input, &data, &size, &w, &h, &t, &oom, &status)) {
      if (oom) {
        jniThrow(env, "java/lang/OutOfMemoryError", "MediaCodec output frame");
      } else if (status == android::INVALID_OPERATION) {
        jniThrow(env, "java/lang/IllegalStateException",
                 "codec is released or requires reset after a fatal error");
      }
      return nullptr;
    }
    jobject buffer = NewByteBuffer(env, data, size, !input);
    if (buffer == nullptr) return nullptr;
    int32_t info[26] = {};
    const int cw = (w + 1) / 2;
    info[0] = 1;
    info[1] = 3;
    info[2] = w;
    info[3] = h;
    info[4] = 8;
    info[5] = 8;
    const int32_t planes[3][5] = {{0, 1, w, 1, 1},
                                  {w * h, 1, cw, 2, 2},
                                  {w * h + cw * ((h + 1) / 2), 1, cw, 2, 2}};
    for (int p = 0; p < 3; p++) {
      for (int k = 0; k < 5; k++) info[6 + p * 5 + k] = planes[p][k];
    }
    jbyteArray infoBytes = env->NewByteArray(sizeof(info));
    env->SetByteArrayRegion(infoBytes, 0, sizeof(info), reinterpret_cast<const jbyte*>(info));
    jclass bb = gJ.byte_buffer;
    jobject infoBuffer = env->CallStaticObjectMethod(
        bb, env->GetStaticMethodID(bb, "wrap", "([B)Ljava/nio/ByteBuffer;"), infoBytes);
    env->DeleteLocalRef(env->CallObjectMethod(infoBuffer, gJ.order, gJ.native_order));
    jobject crop = env->NewObject(gJ.rect, gJ.rect_ctor, 0, 0, w, h);
    jobject image =
        env->NewObject(gJ.media_image, gJ.media_image_ctor, buffer, infoBuffer,
                       input ? JNI_FALSE : JNI_TRUE, static_cast<jlong>(t * 1000), 0, 0, crop);
    env->DeleteLocalRef(buffer);
    env->DeleteLocalRef(infoBuffer);
    env->DeleteLocalRef(infoBytes);
    env->DeleteLocalRef(crop);
    return image;
  }

  jobject InputFormatMap(JNIEnv* env) {
    Format f;
    int status = android::OK;
    if (!engine_->GetInputFormat(&f, &status)) {
      jniThrow(env, "java/lang/IllegalStateException",
               "codec is released or requires reset after a fatal error");
      return nullptr;
    }
    return FormatToMap(env, f);
  }

  jobject OutputFormatMap(JNIEnv* env) {
    Format f;
    int status = android::OK;
    if (!engine_->GetOutputFormat(&f, &status)) {
      jniThrow(env, "java/lang/IllegalStateException",
               "codec is released or requires reset after a fatal error");
      return nullptr;
    }
    return FormatToMap(env, f);
  }

 private:
  static SurfaceDeliverHook MakeReaderHook(JNIEnv* env, jobject surface, ANativeWindow* window) {
    if (surface == nullptr || window != nullptr || !IsReaderSurface(env, surface)) return nullptr;
    auto reader = std::make_shared<GlobalRef>(env, surface);
    return [reader](const VideoFrame& f, int64_t timestamp_ns) {
      return DeliverToReader(reader->get(), f, timestamp_ns);
    };
  }

  static void PostEvent(const std::weak_ptr<CodecEngine>& engine,
                        const std::shared_ptr<WeakRef>& self, uint64_t generation,
                        int what, int index, jobject obj) {
    const auto owner = engine.lock();
    if (!owner) return;
    JNIEnv* env = AttachedEnv("gesher-codec");
    if (env == nullptr) return;
    jobject s = env->NewLocalRef(self->get());
    if (s == nullptr) return;
    jobject listener = env->GetObjectField(s, gJ.listener_lock);
    if (listener != nullptr && env->MonitorEnter(listener) == JNI_OK) {
      // AOSP postEventFromNative only enqueues under this reentrant monitor.
      // Take it before the engine mutex: setOnFrameRenderedListener holds it
      // while calling native code. The epoch check and post then linearize
      // together with native flush/stop/release, without a check/post race.
      owner->EnqueueCallbackIfCurrent(generation, [&] {
        env->CallVoidMethod(s, gJ.post_event, kEventCallback, what, index, obj);
      });
      env->MonitorExit(listener);
    }
    CheckException(env, "MediaCodec.postEventFromNative");
    env->DeleteLocalRef(listener);
    env->DeleteLocalRef(s);
  }

  std::shared_ptr<CodecEngine> engine_;
  std::shared_ptr<WeakRef> self_;
  std::mutex jni_mu_;
  std::shared_ptr<InputSurface> java_input_;
  std::shared_ptr<PersistentInputSurface> persistent_input_;
};

std::mutex gCodecContextLock;
using CodecOwner = std::shared_ptr<Codec>;

CodecOwner GetCodec(JNIEnv* env, jobject thiz) {
  CodecOwner c;
  {
    std::lock_guard<std::mutex> lock(gCodecContextLock);
    auto* holder = reinterpret_cast<CodecOwner*>(env->GetLongField(thiz, gJ.context));
    if (holder != nullptr) c = *holder;
  }
  if (c == nullptr) jniThrow(env, "java/lang/IllegalStateException", nullptr);
  return c;
}

// --- MediaCodec's natives ----------------------------------------------------------------

void JNICALL MC_init(JNIEnv* env, jclass clazz) {
  gJ.context = env->GetFieldID(clazz, "mNativeContext", "J");
  gJ.post_event = env->GetMethodID(clazz, "postEventFromNative", "(IIILjava/lang/Object;)V");
  gJ.listener_lock = env->GetFieldID(clazz, "mListenerLock", "Ljava/lang/Object;");
  auto global = [&](const char* name) {
    jclass c = env->FindClass(name);
    jclass g = c != nullptr ? static_cast<jclass>(env->NewGlobalRef(c)) : nullptr;
    env->DeleteLocalRef(c);
    return g;
  };
  gJ.buffer_info = global("android/media/MediaCodec$BufferInfo");
  gJ.buffer_info_ctor = env->GetMethodID(gJ.buffer_info, "<init>", "()V");
  gJ.buffer_info_set = env->GetMethodID(gJ.buffer_info, "set", "(IIJI)V");
  gJ.codec_exception = global("android/media/MediaCodec$CodecException");
  gJ.codec_exception_ctor = env->GetMethodID(gJ.codec_exception, "<init>", "(IILjava/lang/String;)V");
  gJ.media_image = global("android/media/MediaCodec$MediaImage");
  gJ.media_image_ctor = env->GetMethodID(
      gJ.media_image, "<init>",
      "(Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;ZJIILandroid/graphics/Rect;)V");
  gJ.rect = global("android/graphics/Rect");
  gJ.rect_ctor = env->GetMethodID(gJ.rect, "<init>", "(IIII)V");
  gJ.byte_buffer = global("java/nio/ByteBuffer");
  gJ.as_read_only = env->GetMethodID(gJ.byte_buffer, "asReadOnlyBuffer", "()Ljava/nio/ByteBuffer;");
  gJ.order = env->GetMethodID(gJ.byte_buffer, "order", "(Ljava/nio/ByteOrder;)Ljava/nio/ByteBuffer;");
  gJ.position = env->GetMethodID(gJ.byte_buffer, "position", "(I)Ljava/nio/ByteBuffer;");
  gJ.limit = env->GetMethodID(gJ.byte_buffer, "limit", "(I)Ljava/nio/ByteBuffer;");
  gJ.clear = env->GetMethodID(gJ.byte_buffer, "clear", "()Ljava/nio/ByteBuffer;");
  if (gJ.position == nullptr || gJ.limit == nullptr || gJ.clear == nullptr) {
    env->ExceptionClear();
    gJ.position = env->GetMethodID(gJ.byte_buffer, "position", "(I)Ljava/nio/Buffer;");
    gJ.limit = env->GetMethodID(gJ.byte_buffer, "limit", "(I)Ljava/nio/Buffer;");
    gJ.clear = env->GetMethodID(gJ.byte_buffer, "clear", "()Ljava/nio/Buffer;");
  }
  jclass bo = env->FindClass("java/nio/ByteOrder");
  jobject native_order =
      env->CallStaticObjectMethod(bo, env->GetStaticMethodID(bo, "nativeOrder", "()Ljava/nio/ByteOrder;"));
  gJ.native_order = env->NewGlobalRef(native_order);
  gJ.hash_map = global("java/util/HashMap");
  gJ.hash_map_ctor = env->GetMethodID(gJ.hash_map, "<init>", "()V");
  gJ.hash_map_put =
      env->GetMethodID(gJ.hash_map, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
  gJ.persistable_bundle = global("android/os/PersistableBundle");
  if (gJ.persistable_bundle != nullptr) {
    gJ.persistable_bundle_ctor = env->GetMethodID(gJ.persistable_bundle, "<init>", "()V");
    gJ.bundle_put_int = env->GetMethodID(gJ.persistable_bundle, "putInt", "(Ljava/lang/String;I)V");
    gJ.bundle_put_string =
        env->GetMethodID(gJ.persistable_bundle, "putString", "(Ljava/lang/String;Ljava/lang/String;)V");
  }
  CheckException(env, "MediaCodec.native_init");
}

void JNICALL MC_setup(JNIEnv* env, jobject thiz, jstring name, jboolean name_is_type, jboolean encoder,
                      jint, jint) {
  if (name == nullptr) {
    jniThrow(env, "java/lang/NullPointerException", "No codec name specified");
    return;
  }
  std::string n = Utf8(env, name);
  const CodecEntry* entry =
      name_is_type ? CodecEngine::FindByType(n, encoder) : CodecEngine::FindByName(n);
  if (entry == nullptr) {
    if (name_is_type) {
      jniThrow(env, "java/io/IOException", ("Failed to find matching codec " + n).c_str());
    } else {
      jniThrow(env, "java/lang/IllegalArgumentException",
               ("Failed to initialize " + n + ", error 0xffffffc8 (NAME_NOT_FOUND)").c_str());
    }
    return;
  }
  auto holder = std::make_unique<CodecOwner>(std::make_shared<Codec>(env, thiz, *entry));
  {
    std::lock_guard<std::mutex> lock(gCodecContextLock);
    env->SetLongField(thiz, gJ.context, reinterpret_cast<jlong>(holder.release()));
  }
}

void JNICALL MC_release(JNIEnv* env, jobject thiz) {
  std::unique_ptr<CodecOwner> holder;
  {
    std::lock_guard<std::mutex> lock(gCodecContextLock);
    holder.reset(reinterpret_cast<CodecOwner*>(env->GetLongField(thiz, gJ.context)));
    env->SetLongField(thiz, gJ.context, 0);
  }
  if (holder != nullptr && *holder != nullptr) (*holder)->Release();
}

void JNICALL MC_reset(JNIEnv* env, jobject thiz) {
  if (auto c = GetCodec(env, thiz)) c->Reset();
}

void JNICALL MC_setCallback(JNIEnv* env, jobject thiz, jobject cb) {
  if (auto c = GetCodec(env, thiz); c && !c->SetAsync(cb != nullptr)) {
    jniThrow(env, "java/lang/IllegalStateException", "setCallback() requires an idle codec");
  }
}

void JNICALL MC_configure(JNIEnv* env, jobject thiz, jobjectArray keys, jobjectArray values,
                          jobject surface, jobject crypto, jobject, jint flags) {
  auto c = GetCodec(env, thiz);
  if (c == nullptr) return;
  if (crypto != nullptr) {
    ThrowCodecException(env, -1, "MediaCrypto (DRM) is not supported on Gesher");
    return;
  }
  if (((flags & 1) != 0) != c->entry().encoder) {
    jniThrow(env, "java/lang/IllegalArgumentException",
             c->entry().encoder ? "an encoder needs CONFIGURE_FLAG_ENCODE"
                                : "a decoder cannot encode");
    return;
  }
  std::string error;
  int status = android::OK;
  if (!c->Configure(env, FormatFromJava(env, keys, values), surface, &error, &status)) {
    ThrowCodecStatus(env, status, error);
  }
}

void JNICALL MC_setSurface(JNIEnv* env, jobject thiz, jobject surface) {
  if (auto c = GetCodec(env, thiz)) {
    int status = android::OK;
    c->SetSurface(env, surface, &status);
    ThrowCodecStatus(env, status, "setOutputSurface() requires an initialized codec");
  }
}

void JNICALL MC_start(JNIEnv* env, jobject thiz) {
  auto c = GetCodec(env, thiz);
  if (c == nullptr) return;
  std::string error;
  if (!c->Start(&error)) jniThrow(env, "java/lang/IllegalStateException", error.c_str());
}

void JNICALL MC_stop(JNIEnv* env, jobject thiz) {
  if (auto c = GetCodec(env, thiz)) {
    int status = android::OK;
    c->Stop(&status);
    ThrowCodecStatus(env, status, "stop() after a fatal codec error");
  }
}

void JNICALL MC_flush(JNIEnv* env, jobject thiz) {
  if (auto c = GetCodec(env, thiz)) {
    int status = android::OK;
    c->Flush(&status);
    ThrowCodecStatus(env, status, "flush() requires an executing codec");
  }
}

void JNICALL MC_queueInputBuffer(JNIEnv* env, jobject thiz, jint index, jint offset, jint size,
                                 jlong time_us, jint flags) {
  auto c = GetCodec(env, thiz);
  if (c == nullptr) return;
  std::string error;
  if (!c->QueueInput(index, offset, size, time_us, flags, &error)) {
    jniThrow(env, "java/lang/IllegalStateException", error.c_str());
  }
}

jint JNICALL MC_dequeueInputBuffer(JNIEnv* env, jobject thiz, jlong timeout_us) {
  auto c = GetCodec(env, thiz);
  if (c == nullptr) return kInfoTryAgainLater;
  int status = android::OK;
  int index = c->DequeueInput(timeout_us, &status);
  ThrowCodecStatus(env, status, "dequeueInputBuffer() is invalid or was interrupted");
  return index;
}

jint JNICALL MC_dequeueOutputBuffer(JNIEnv* env, jobject thiz, jobject info, jlong timeout_us) {
  auto c = GetCodec(env, thiz);
  if (c == nullptr) return kInfoTryAgainLater;
  int status = android::OK;
  int index = c->DequeueOutput(env, info, timeout_us, &status);
  ThrowCodecStatus(env, status, "dequeueOutputBuffer() is invalid or was interrupted");
  return index;
}

void JNICALL MC_releaseOutputBuffer(JNIEnv* env, jobject thiz, jint index, jboolean render,
                                    jboolean has_timestamp, jlong time_ns) {
  auto c = GetCodec(env, thiz);
  if (c == nullptr) return;
  std::string error;
  int status = android::OK;
  if (!c->ReleaseOutput(index, render, has_timestamp, time_ns, &error, &status)) {
    ThrowCodecStatus(env, status, error);
  }
}

void JNICALL MC_signalEndOfInputStream(JNIEnv* env, jobject thiz) {
  auto c = GetCodec(env, thiz);
  if (c == nullptr) return;
  std::string error;
  if (!c->SignalEndOfInputStream(&error)) {
    jniThrow(env, "java/lang/IllegalStateException", error.c_str());
  }
}

jobject JNICALL MC_createInputSurface(JNIEnv* env, jobject thiz) {
  auto c = GetCodec(env, thiz);
  if (c == nullptr) return nullptr;
  std::string error;
  jobject s = c->CreateInputSurface(env, &error);
  if (s == nullptr && !env->ExceptionCheck()) {
    jniThrow(env, "java/lang/IllegalStateException", error.c_str());
  }
  return s;
}

jobject JNICALL MC_createPersistentInputSurface(JNIEnv* env, jclass) {
  return NewPersistentInputSurface(env);
}

void JNICALL MC_releasePersistentInputSurface(JNIEnv* env, jclass, jobject surface) {
  ReleasePersistentInputSurface(env, surface);
}

void JNICALL MC_setInputSurface(JNIEnv* env, jobject thiz, jobject surface) {
  auto c = GetCodec(env, thiz);
  if (c == nullptr) return;
  auto source = GetPersistentInputSurface(env, surface);
  if (source == nullptr) return;
  std::string error;
  if (!c->SetInputSurface(source, &error)) {
    jniThrow(env, "java/lang/IllegalStateException", error.c_str());
  }
}

jobject JNICALL MC_getFormatNative(JNIEnv* env, jobject thiz, jboolean input) {
  auto c = GetCodec(env, thiz);
  if (c == nullptr) return nullptr;
  return input ? c->InputFormatMap(env) : c->OutputFormatMap(env);
}

jobject JNICALL MC_getOutputFormatNative(JNIEnv* env, jobject thiz, jint) {
  auto c = GetCodec(env, thiz);
  return c != nullptr ? c->OutputFormatMap(env) : nullptr;
}

jobjectArray JNICALL MC_getBuffers(JNIEnv* env, jobject thiz, jboolean input) {
  auto c = GetCodec(env, thiz);
  return c != nullptr ? c->Buffers(env, input) : nullptr;
}

jobject JNICALL MC_getBuffer(JNIEnv* env, jobject thiz, jboolean input, jint index) {
  auto c = GetCodec(env, thiz);
  if (c == nullptr) return nullptr;
  return input ? c->InputBuffer(env, index) : c->OutputBuffer(env, index);
}

jobject JNICALL MC_getImage(JNIEnv* env, jobject thiz, jboolean input, jint index) {
  auto c = GetCodec(env, thiz);
  if (c == nullptr) return nullptr;
  return c->OutputImage(env, index, input);
}

jstring JNICALL MC_getCanonicalName(JNIEnv* env, jobject thiz) {
  auto c = GetCodec(env, thiz);
  return c != nullptr ? env->NewStringUTF(c->entry().name.c_str()) : nullptr;
}

void JNICALL MC_setParameters(JNIEnv* env, jobject thiz, jobjectArray keys, jobjectArray values) {
  auto c = GetCodec(env, thiz);
  if (c == nullptr) return;
  Format parameters = FormatFromJava(env, keys, values);
  if (!env->ExceptionCheck() && !c->SetParameters(parameters)) {
    jniThrow(env, "java/lang/IllegalStateException", "setParameters() after release()");
  }
}

void JNICALL MC_setVideoScalingMode(JNIEnv* env, jobject thiz, jint mode) {
  auto c = GetCodec(env, thiz);
  if (c == nullptr) return;
  if (mode != 1 && mode != 2) {
    jniThrow(env, "java/lang/IllegalArgumentException", "unrecognized video scaling mode");
    return;
  }
  if (!c->SetVideoScalingMode(mode)) {
    jniThrow(env, "java/lang/IllegalStateException", "setVideoScalingMode() after release()");
  }
}

void JNICALL MC_enableOnFrameRenderedListener(JNIEnv* env, jobject thiz, jboolean enable) {
  if (auto c = GetCodec(env, thiz)) c->EnableFrameRendered(enable);
}

jobject JNICALL MC_getMetrics(JNIEnv* env, jobject thiz) {
  auto codec = GetCodec(env, thiz);
  return codec != nullptr ? codec->GetMetrics(env) : nullptr;
}

const JNINativeMethod kCodecMethods[] = {
    {"native_init", "()V", GESHER_NATIVE(MC_init)},
    {"native_setup", "(Ljava/lang/String;ZZII)V", GESHER_NATIVE(MC_setup)},
    {"native_finalize", "()V", GESHER_NATIVE(MC_release)},
    {"native_release", "()V", GESHER_NATIVE(MC_release)},
    {"native_reset", "()V", GESHER_NATIVE(MC_reset)},
    {"native_setCallback", "(Landroid/media/MediaCodec$Callback;)V", GESHER_NATIVE(MC_setCallback)},
    {"native_configure",
     "([Ljava/lang/String;[Ljava/lang/Object;Landroid/view/Surface;Landroid/media/MediaCrypto;"
     "Landroid/os/IHwBinder;I)V",
     GESHER_NATIVE(MC_configure)},
    {"native_setSurface", "(Landroid/view/Surface;)V", GESHER_NATIVE(MC_setSurface)},
    {"native_start", "()V", GESHER_NATIVE(MC_start)},
    {"native_stop", "()V", GESHER_NATIVE(MC_stop)},
    {"native_flush", "()V", GESHER_NATIVE(MC_flush)},
    {"native_queueInputBuffer", "(IIIJI)V", GESHER_NATIVE(MC_queueInputBuffer)},
    {"native_dequeueInputBuffer", "(J)I", GESHER_NATIVE(MC_dequeueInputBuffer)},
    {"native_dequeueOutputBuffer", "(Landroid/media/MediaCodec$BufferInfo;J)I",
     GESHER_NATIVE(MC_dequeueOutputBuffer)},
    {"releaseOutputBuffer", "(IZZJ)V", GESHER_NATIVE(MC_releaseOutputBuffer)},
    {"signalEndOfInputStream", "()V", GESHER_NATIVE(MC_signalEndOfInputStream)},
    {"createInputSurface", "()Landroid/view/Surface;", GESHER_NATIVE(MC_createInputSurface)},
    {"native_createPersistentInputSurface", "()Landroid/media/MediaCodec$PersistentSurface;",
     GESHER_NATIVE(MC_createPersistentInputSurface)},
    {"native_releasePersistentInputSurface", "(Landroid/view/Surface;)V",
     GESHER_NATIVE(MC_releasePersistentInputSurface)},
    {"native_setInputSurface", "(Landroid/view/Surface;)V", GESHER_NATIVE(MC_setInputSurface)},
    {"getFormatNative", "(Z)Ljava/util/Map;", GESHER_NATIVE(MC_getFormatNative)},
    {"getOutputFormatNative", "(I)Ljava/util/Map;", GESHER_NATIVE(MC_getOutputFormatNative)},
    {"getBuffers", "(Z)[Ljava/nio/ByteBuffer;", GESHER_NATIVE(MC_getBuffers)},
    {"getBuffer", "(ZI)Ljava/nio/ByteBuffer;", GESHER_NATIVE(MC_getBuffer)},
    {"getImage", "(ZI)Landroid/media/Image;", GESHER_NATIVE(MC_getImage)},
    {"getCanonicalName", "()Ljava/lang/String;", GESHER_NATIVE(MC_getCanonicalName)},
    {"setParameters", "([Ljava/lang/String;[Ljava/lang/Object;)V", GESHER_NATIVE(MC_setParameters)},
    {"setVideoScalingMode", "(I)V", GESHER_NATIVE(MC_setVideoScalingMode)},
    {"native_enableOnFrameRenderedListener", "(Z)V",
     GESHER_NATIVE(MC_enableOnFrameRenderedListener)},
    {"native_getMetrics", "()Landroid/os/PersistableBundle;", GESHER_NATIVE(MC_getMetrics)},
};

// --- MediaCodecList ----------------------------------------------------------------------

const CodecEntry* EntryAt(JNIEnv* env, jint index) {
  const auto& codecs = CodecEngine::Codecs();
  if (index < 0 || static_cast<size_t>(index) >= codecs.size()) {
    jniThrow(env, "java/lang/IllegalArgumentException", nullptr);
    return nullptr;
  }
  return &codecs[static_cast<size_t>(index)];
}

jint JNICALL MCL_getCodecCount(JNIEnv*, jclass) {
  return static_cast<jint>(CodecEngine::Codecs().size());
}

jstring JNICALL MCL_getCodecName(JNIEnv* env, jclass, jint index) {
  const CodecEntry* e = EntryAt(env, index);
  return e != nullptr ? env->NewStringUTF(e->name.c_str()) : nullptr;
}

jint JNICALL MCL_getAttributes(JNIEnv* env, jclass, jint index) {
  const CodecEntry* e = EntryAt(env, index);
  if (e == nullptr) return 0;
  return (e->encoder ? 1 : 0) | (e->hardware ? 8 : 4);
}

jobjectArray JNICALL MCL_getSupportedTypes(JNIEnv* env, jclass, jint index) {
  const CodecEntry* e = EntryAt(env, index);
  if (e == nullptr) return nullptr;
  jobjectArray a = env->NewObjectArray(1, env->FindClass("java/lang/String"), nullptr);
  jstring s = env->NewStringUTF(e->mime.c_str());
  env->SetObjectArrayElement(a, 0, s);
  env->DeleteLocalRef(s);
  return a;
}

jint JNICALL MCL_findCodecByName(JNIEnv* env, jclass, jstring name) {
  if (name == nullptr) {
    jniThrow(env, "java/lang/IllegalArgumentException", nullptr);
    return -2;
  }
  std::string n = Utf8(env, name);
  const auto& codecs = CodecEngine::Codecs();
  for (size_t i = 0; i < codecs.size(); i++) {
    if (codecs[i].name == n) return static_cast<jint>(i);
  }
  return -2;
}

std::vector<std::pair<int, int>> ProfileLevels(const std::string& mime, bool encoder,
                                               int avc_encoder_profiles) {
  if (mime == "video/avc") {
    if (!encoder) {
      return {{0x01, 0x8000}, {0x02, 0x8000}, {0x08, 0x8000}, {0x10000, 0x8000}, {0x80000, 0x8000}};
    }
    std::vector<std::pair<int, int>> profiles;
    for (int profile : {kAvcProfileBaseline, kAvcProfileMain, kAvcProfileHigh}) {
      if ((avc_encoder_profiles & profile) != 0) profiles.emplace_back(profile, kAvcLevel41);
    }
    return profiles;
  }
  if (mime == "video/hevc") return {{0x01, 0x10000}};
  if (mime == "video/x-vnd.on2.vp8") return {{0x01, 0x01}};
  if (mime == "video/x-vnd.on2.vp9") return {{0x01, 0x200}};
  if (mime == "video/av01") return {{0x1, 0x2000}};
  if (mime == "video/mp4v-es") return {{0x01, 0x80}};
  if (mime == "video/3gpp") return {{0x01, 0x80}};
  if (mime == "audio/mp4a-latm") {
    if (encoder) return {{2, 0}};
    return {{2, 0}, {5, 0}, {29, 0}};
  }
  return {};
}

// Local helper for JavaMap with strings+ints (used only by MediaCodecList).
struct CapsMap {
  std::map<std::string, int32_t> ints;
  std::map<std::string, std::string> strings;

  jobject Make(JNIEnv* env) const {
    jobject map = env->NewObject(gJ.hash_map, gJ.hash_map_ctor);
    jclass integer = env->FindClass("java/lang/Integer");
    jmethodID int_val = env->GetStaticMethodID(integer, "valueOf", "(I)Ljava/lang/Integer;");
    auto put = [&](const std::string& k, jobject v) {
      jstring key = env->NewStringUTF(k.c_str());
      env->DeleteLocalRef(env->CallObjectMethod(map, gJ.hash_map_put, key, v));
      env->DeleteLocalRef(key);
      env->DeleteLocalRef(v);
    };
    for (const auto& [k, v] : ints) put(k, env->CallStaticObjectMethod(integer, int_val, v));
    for (const auto& [k, v] : strings) put(k, env->NewStringUTF(v.c_str()));
    return map;
  }
};

jobject JNICALL MCL_getCodecCapabilities(JNIEnv* env, jclass, jint index, jstring type) {
  const CodecEntry* e = EntryAt(env, index);
  if (e == nullptr) return nullptr;
  if (type == nullptr || Utf8(env, type) != e->mime) {
    jniThrow(env, "java/lang/IllegalArgumentException", nullptr);
    return nullptr;
  }
  jclass pl_class = env->FindClass("android/media/MediaCodecInfo$CodecProfileLevel");
  auto pls = ProfileLevels(e->mime, e->encoder, e->avc_encoder_profiles);
  jobjectArray pl_array = env->NewObjectArray(static_cast<jsize>(pls.size()), pl_class, nullptr);
  jfieldID profile = env->GetFieldID(pl_class, "profile", "I");
  jfieldID level = env->GetFieldID(pl_class, "level", "I");
  for (size_t i = 0; i < pls.size(); i++) {
    jobject o = env->AllocObject(pl_class);
    env->SetIntField(o, profile, pls[i].first);
    env->SetIntField(o, level, pls[i].second);
    env->SetObjectArrayElement(pl_array, static_cast<jsize>(i), o);
    env->DeleteLocalRef(o);
  }
  std::vector<jint> colors;
  if (e->video) {
    colors = {kCodecColorFormatYuv420Flexible, kCodecColorFormatYuv420Planar,
              kCodecColorFormatSurface};
  }
  if (e->video && e->encoder) colors.push_back(kCodecColorFormatYuv420SemiPlanar);
  jintArray color_array = env->NewIntArray(static_cast<jsize>(colors.size()));
  if (!colors.empty()) {
    env->SetIntArrayRegion(color_array, 0, static_cast<jsize>(colors.size()), colors.data());
  }

  CapsMap defaults;
  defaults.strings["mime"] = e->mime;
  CapsMap details;
  if (e->video) {
    const int mw = e->max_width > 0 ? e->max_width : 1920;
    const int mh = e->max_height > 0 ? e->max_height : 1088;
    details.strings["size-range"] = "2x2-" + std::to_string(mw) + "x" + std::to_string(mh);
    details.strings["alignment"] = "2x2";
    const bool small_blocks =
        e->mime == "video/hevc" || e->mime == "video/x-vnd.on2.vp9" || e->mime == "video/av01";
    const int bs = small_blocks ? 8 : 16;
    details.strings["block-size"] = small_blocks ? "8x8" : "16x16";
    const int64_t blocks = static_cast<int64_t>((mw + bs - 1) / bs) * ((mh + bs - 1) / bs);
    details.strings["block-count-range"] = "1-" + std::to_string(blocks);
    details.strings["blocks-per-second-range"] = "1-" + std::to_string(blocks * 60);
    details.strings["bitrate-range"] = "1-120000000";
    details.strings["frame-rate-range"] = "1-120";
    details.ints["feature-adaptive-playback"] = 1;
  } else {
    details.strings["max-channel-count"] = e->encoder ? "2" : "8";
    details.strings["sample-rate-ranges"] = !e->encoder                 ? "7350-96000"
                                            : e->mime == "audio/opus" ? "8000,12000,16000,24000,48000"
                                                                      : "44100,48000";
    details.strings["bitrate-range"] =
        !e->encoder ? "8000-960000" : e->mime == "audio/opus" ? "6000-510000" : "96000-192000";
  }
  jobject default_map = defaults.Make(env);
  jobject details_map = details.Make(env);
  jclass impl_class = env->FindClass("android/media/MediaCodecInfo$CodecCapabilities$CodecCapsLegacyImpl");
  jobject impl = nullptr;
  if (impl_class == nullptr) env->ExceptionClear();
  if (impl_class != nullptr) {
    jmethodID ctor = env->GetMethodID(impl_class, "<init>",
                                      "([Landroid/media/MediaCodecInfo$CodecProfileLevel;[IZ"
                                      "Ljava/util/Map;Ljava/util/Map;)V");
    impl = env->NewObject(impl_class, ctor, pl_array, color_array,
                          e->encoder ? JNI_TRUE : JNI_FALSE, default_map, details_map);
    if (env->ExceptionCheck()) {
      CheckException(env, ("MediaCodecList.getCodecCapabilities(" + e->name + ")").c_str());
      env->DeleteLocalRef(default_map);
      env->DeleteLocalRef(details_map);
      env->DeleteLocalRef(pl_array);
      env->DeleteLocalRef(color_array);
      return nullptr;
    }
  }
  jobject caps = nullptr;
  CheckException(env, "MediaCodecList: CodecCapsLegacyImpl");
  jclass caps_class = env->FindClass("android/media/MediaCodecInfo$CodecCapabilities");
  if (caps_class == nullptr) {
    env->ExceptionClear();
    return nullptr;
  }
  if (impl != nullptr) {
    jmethodID ctor = env->GetMethodID(
        caps_class, "<init>", "(Landroid/media/MediaCodecInfo$CodecCapabilities$CodecCapsIntf;)V");
    caps = env->NewObject(caps_class, ctor, impl);
  } else {
    env->ExceptionClear();
    jmethodID ctor = env->GetMethodID(caps_class, "<init>",
                                      "([Landroid/media/MediaCodecInfo$CodecProfileLevel;[IZ"
                                      "Ljava/util/Map;Ljava/util/Map;)V");
    caps = env->NewObject(caps_class, ctor, pl_array, color_array,
                          e->encoder ? JNI_TRUE : JNI_FALSE, default_map, details_map);
  }
  CheckException(env, "MediaCodecList.getCodecCapabilities");
  env->DeleteLocalRef(default_map);
  env->DeleteLocalRef(details_map);
  env->DeleteLocalRef(pl_array);
  env->DeleteLocalRef(color_array);
  if (impl != nullptr) env->DeleteLocalRef(impl);
  return caps;
}

jobject JNICALL MCL_getGlobalSettings(JNIEnv* env, jclass) { return CapsMap().Make(env); }

void JNICALL MCL_init(JNIEnv*, jclass) {}

const JNINativeMethod kListMethods[] = {
    {"native_getCodecCount", "()I", GESHER_NATIVE(MCL_getCodecCount)},
    {"getCanonicalName", "(I)Ljava/lang/String;", GESHER_NATIVE(MCL_getCodecName)},
    {"getCodecName", "(I)Ljava/lang/String;", GESHER_NATIVE(MCL_getCodecName)},
    {"getAttributes", "(I)I", GESHER_NATIVE(MCL_getAttributes)},
    {"getSupportedTypes", "(I)[Ljava/lang/String;", GESHER_NATIVE(MCL_getSupportedTypes)},
    {"getCodecCapabilities", "(ILjava/lang/String;)Landroid/media/MediaCodecInfo$CodecCapabilities;",
     GESHER_NATIVE(MCL_getCodecCapabilities)},
    {"native_getGlobalSettings", "()Ljava/util/Map;", GESHER_NATIVE(MCL_getGlobalSettings)},
    {"findCodecByName", "(Ljava/lang/String;)I", GESHER_NATIVE(MCL_findCodecByName)},
    {"native_init", "()V", GESHER_NATIVE(MCL_init)},
};

}  // namespace

Format FormatFromJavaArrays(JNIEnv* env, jobjectArray keys, jobjectArray values) {
  return FormatFromJava(env, keys, values);
}

int RegisterMediaCodec(JNIEnv* env) {
  if (RegisterStandins(env, "android/media/MediaCodec")) {
    RegisterMethods(env, "android/media/MediaCodec", kCodecMethods);
  }
  if (RegisterStandins(env, "android/media/MediaCodecList")) {
    RegisterMethods(env, "android/media/MediaCodecList", kListMethods);
  }
  if (jclass mc = env->FindClass("android/media/MediaCodec")) {
    MC_init(env, mc);
    env->DeleteLocalRef(mc);
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  return 0;
}

}  // namespace gesher::media
