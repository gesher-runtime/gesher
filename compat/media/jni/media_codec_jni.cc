// Gesher: android.media.MediaCodec and MediaCodecList over Gesher's decoders
// (docs/media.md "Video", ADR 0051), in libmedia_jni.so.
//
// A codec is a decoder (audio: compat/media/player's AudioDecoder; video:
// compat/media/video's VideoDecoder) behind MediaCodec's buffer model: a
// fixed set of input and output slots in native memory that Java sees as
// direct ByteBuffers, a worker thread that decodes queued inputs into free
// output slots, synchronous dequeue calls or, with a callback set,
// asynchronous events (postEventFromNative, EVENT_CALLBACK). Video comes
// out as I420 (COLOR_FormatYUV420Planar, also as an Image), or, with an
// output Surface, is drawn into it (RGBA through ANativeWindow_lock) when
// the app releases the buffer with render = true; an ImageReader's Surface
// gets the frame as an Image directly through the native reader queue
// (media3, ADR 0059), without a Java byte-array round trip.
//
// Encoders (media3, ADR 0059) are compat/media/encode's behind the same slots:
// raw input (I420 or NV12 video, 16-bit or float PCM) in ByteBuffers or Images,
// or video from an input Surface (createInputSurface: a QueueSurface whose
// frames, RGBA, are converted to I420); out come the codec-config buffer first
// (BUFFER_FLAG_CODEC_CONFIG: H.264's parameter sets, AAC's
// AudioSpecificConfig, the OpusHead), then access units flagged key frames
// where they are, with the output format's csd-0 (and csd-1) set.

#include <jni.h>
#include <gesher/clocks.h>
#include <gesher/perf_trace.h>

#include <media/NdkMediaFormat.h>
#include <utils/Errors.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../audio/log.h"
#include "../player/decoder.h"
#include "../player/extractor.h"
#include "../encode/encoder.h"
#include "../video/input_surface.h"
#include "../video/video_decoder.h"
#include "../../devices/camera_frames.h"
#include "jni_util.h"
#include "media_jni.h"

// The ANativeWindow functions dalvikvm defines (docs/graphics.md) and
// libandroid_runtime's host ANativeWindow_fromSurface.
struct ANativeWindow;
extern "C" {
ANativeWindow* ANativeWindow_fromSurface(JNIEnv* env, jobject surface);
void ANativeWindow_acquire(ANativeWindow* window);
void ANativeWindow_release(ANativeWindow* window);
}

namespace gesher::media {
namespace {

// MediaCodec's constants.
constexpr int kInfoTryAgainLater = -1;
constexpr int kInfoOutputFormatChanged = -2;
constexpr int kFlagKeyFrame = 1;
constexpr int kFlagCodecConfig = 2;
constexpr int kFlagEndOfStream = 4;
constexpr int kEventCallback = 1;
constexpr int kEventFrameRendered = 3;
constexpr int kCbInputAvailable = 1;
constexpr int kCbOutputAvailable = 2;
constexpr int kCbError = 3;
constexpr int kCbOutputFormatChange = 4;
constexpr int kColorFormatYuv420Planar = 19;
constexpr int kColorFormatYuv420SemiPlanar = 21;
constexpr int kColorFormatYuv420Flexible = 0x7F420888;
constexpr int kColorFormatSurface = 0x7F000789;
constexpr int kSlots = 8;

// --- The codecs this machine has ------------------------------------------------------------

struct CodecEntry {
  std::string name;
  std::string mime;
  bool video;
  bool hardware;
  int max_width, max_height;
  bool encoder = false;
};

std::string ShortName(const std::string& mime) {
  static const std::map<std::string, std::string> names = {
      {"video/avc", "avc"},         {"video/hevc", "hevc"},
      {"video/x-vnd.on2.vp8", "vp8"}, {"video/x-vnd.on2.vp9", "vp9"},
      {"video/av01", "av1"},        {"video/mp4v-es", "mpeg4"},
      {"video/3gpp", "h263"},       {"audio/mpeg", "mp3"},
      {"audio/mp4a-latm", "aac"},   {"audio/vorbis", "vorbis"},
      {"audio/opus", "opus"},       {"audio/raw", "raw"},
      {"audio/g711-mlaw", "g711.mlaw"}, {"audio/g711-alaw", "g711.alaw"},
  };
  auto it = names.find(mime);
  return it != names.end() ? it->second : mime;
}

const std::vector<CodecEntry>& Codecs() {
  static const std::vector<CodecEntry> codecs = [] {
    std::vector<CodecEntry> all;
    for (const VideoCodecInfo& v : VideoDecoder::Available()) {
      all.push_back({"c2.gesher." + ShortName(v.mime) + ".decoder", v.mime, true, v.hardware,
                     v.max_width, v.max_height});
    }
    for (const char* mime : {"audio/mp4a-latm", "audio/mpeg", "audio/vorbis", "audio/opus",
                             "audio/raw", "audio/g711-mlaw", "audio/g711-alaw"}) {
      if (AudioDecoder::Supports(mime)) {
        all.push_back({"c2.gesher." + ShortName(mime) + ".decoder", mime, false, false, 0, 0});
      }
    }
    // The encoders (compat/media/encode).
    for (const EncoderInfo& e : AvailableEncoders()) {
      all.push_back({"c2.gesher." + ShortName(e.mime) + ".encoder", e.mime, e.video, false,
                     e.video ? 1920 : 0, e.video ? 1088 : 0, true});
    }
    return all;
  }();
  return codecs;
}

const CodecEntry* FindByName(const std::string& name) {
  for (const CodecEntry& c : Codecs()) {
    if (c.name == name) return &c;
  }
  return nullptr;
}

const CodecEntry* FindByType(const std::string& mime, bool encoder) {
  for (const CodecEntry& c : Codecs()) {
    if (c.mime == mime && c.encoder == encoder) return &c;
  }
  return nullptr;
}

// --- Java helpers ---------------------------------------------------------------------------

struct {
  jfieldID context;        // MediaCodec.mNativeContext
  jmethodID post_event;    // MediaCodec.postEventFromNative(int, int, int, Object)
  jclass buffer_info;      // MediaCodec$BufferInfo
  jmethodID buffer_info_ctor;
  jmethodID buffer_info_set;
  jclass codec_exception;  // MediaCodec$CodecException
  jmethodID codec_exception_ctor;
  jclass media_image;      // MediaCodec$MediaImage
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

struct CodecMetrics {
  int32_t width = 0, height = 0, rotation = 0;
  int32_t sample_rate = 0, channel_count = 0;
  int32_t bitrate = 0;
};

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
  // These component properties are known at allocation, before configure.
  put_int("secure", 0);  // Configure rejects protected sessions.
  put_int("hardware", entry.hardware ? 1 : 0);
  if (!metrics) return bundle;
  put_string("mime", entry.mime.c_str());
  put_int("encoder", entry.encoder ? 1 : 0);
  if (entry.video) {
    if (metrics->width > 0) put_int("width", metrics->width);
    if (metrics->height > 0) put_int("height", metrics->height);
    put_int("rotation", metrics->rotation);          // MediaCodec.MetricsConstants.ROTATION
    put_int("rotation-degrees", metrics->rotation);  // Stagefright's metric spelling
  } else {
    if (metrics->sample_rate > 0) put_int("sampleRate", metrics->sample_rate);
    if (metrics->channel_count > 0) put_int("channelCount", metrics->channel_count);
  }
  if (metrics->bitrate > 0) put_int("bitrate", metrics->bitrate);
  return bundle;
}

jobject Box(JNIEnv* env, int32_t v) {
  jclass c = env->FindClass("java/lang/Integer");
  return env->CallStaticObjectMethod(c, env->GetStaticMethodID(c, "valueOf", "(I)Ljava/lang/Integer;"), v);
}
jobject Box(JNIEnv* env, int64_t v) {
  jclass c = env->FindClass("java/lang/Long");
  return env->CallStaticObjectMethod(c, env->GetStaticMethodID(c, "valueOf", "(J)Ljava/lang/Long;"),
                                     static_cast<jlong>(v));
}

// A Java Map of strings to Integer, Long and String values.
struct JavaMap {
  std::map<std::string, int32_t> ints;
  std::map<std::string, int64_t> longs;
  std::map<std::string, std::string> strings;

  jobject Make(JNIEnv* env) const {
    jobject map = env->NewObject(gJ.hash_map, gJ.hash_map_ctor);
    auto put = [&](const std::string& k, jobject v) {
      jstring key = env->NewStringUTF(k.c_str());
      env->DeleteLocalRef(env->CallObjectMethod(map, gJ.hash_map_put, key, v));
      env->DeleteLocalRef(key);
      env->DeleteLocalRef(v);
    };
    for (const auto& [k, v] : ints) put(k, Box(env, v));
    for (const auto& [k, v] : longs) put(k, Box(env, v));
    for (const auto& [k, v] : strings) put(k, env->NewStringUTF(v.c_str()));
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
      env->NewObject(gJ.codec_exception, gJ.codec_exception_ctor, error, 0 /* Java fatal action */, m));
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

// A MediaFormat's keys and values (Integer, Long, Float, String, ByteBuffer)
// as an AMediaFormat.
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
        for (jint j = 0; j < size; j++) bytes[static_cast<size_t>(j)] = static_cast<uint8_t>(env->CallByteMethod(v, get, start + j));
      }
      AMediaFormat_setBuffer(f.get(), key.c_str(), bytes.data(), bytes.size());
    }
    env->DeleteLocalRef(k);
    env->DeleteLocalRef(v);
  }
  return f;
}

// --- ImageReader surfaces ------------------------------------------------------------------

// com.gesher.server.CameraFrames (gesher-app.jar, the boot class path): an ImageReader's
// Surface is Gesher's in-process queue (compat/devices/camera_frames.cc).
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

// A decoded frame into an ImageReader, directly from native I420 storage.
int DeliverToReader(jobject surface, const VideoFrame& f, int64_t timestamp_ns) {
  JNIEnv* env = AttachedEnv("gesher-codec");
  if (env == nullptr) return android::UNKNOWN_ERROR;
  const int result = android::QueueImageReaderVideoFrame(env, surface, f, timestamp_ns);
  if (CheckException(env, "ImageReader native delivery")) return android::UNKNOWN_ERROR;
  // CameraFrames distinguishes an unsupported format (1) from a closed or
  // missing consumer (NO_INIT); only the latter invalidates MediaCodec output.
  return result == 1 ? android::BAD_VALUE : result;
}

// --- A codec -----------------------------------------------------------------------------

struct Slot {
  std::vector<uint8_t> mem;
  bool client = false;  // dequeued by the app
  // Inputs: what was queued. Outputs: what is in it.
  int offset = 0, size = 0;
  int64_t time_us = 0;
  int flags = 0;
  std::shared_ptr<VideoFrame> frame;  // video outputs
  std::unique_ptr<uint8_t[]> deferred_mem;  // CPU access after removing a Surface
};

// CPU output exposes the visible part as tightly packed I420. Surface output
// keeps the decoded frame instead: neither getOutputBuffer nor getOutputImage
// exposes its bytes, and releaseOutputBuffer renders the owned frame directly.
void CopyOutputFramePixels(const VideoFrame& f, uint8_t* d) {
  const int w = f.visible_width(), h = f.visible_height();
  const int cw = (w + 1) / 2, ch = (h + 1) / 2;
  for (int y = 0; y < h; y++) {
    memcpy(d + static_cast<size_t>(y) * w,
           f.y() + static_cast<size_t>(f.crop_top + y) * f.stride_y + f.crop_left, static_cast<size_t>(w));
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
  return static_cast<size_t>(w) * h
      + 2 * static_cast<size_t>((w + 1) / 2) * ((h + 1) / 2);
}

void CopyOutputFrameToBuffer(Slot& slot) {
  const VideoFrame& f = *slot.frame;
  const size_t size = OutputFrameBytes(f);
  if (slot.mem.size() < size) slot.mem.assign(size, 0);
  CopyOutputFramePixels(f, slot.mem.data());
}

uint8_t* CpuOutputBytes(JNIEnv* env, Slot& slot) {
  if (!slot.mem.empty()) return slot.mem.data();
  if (slot.frame == nullptr) return nullptr;
  if (slot.deferred_mem == nullptr) {
    // This JNI target is built without C++ exceptions. Unlike the worker's
    // existing vector allocation, a lazy accessor must report Java OOM.
    slot.deferred_mem.reset(new (std::nothrow) uint8_t[OutputFrameBytes(*slot.frame)]);
    if (slot.deferred_mem == nullptr) {
      jniThrow(env, "java/lang/OutOfMemoryError", "MediaCodec output frame");
      return nullptr;
    }
    CopyOutputFramePixels(*slot.frame, slot.deferred_mem.get());
  }
  return slot.deferred_mem.get();
}

class Codec {
 public:
  Codec(JNIEnv* env, jobject thiz, const CodecEntry& entry)
      : entry_(entry), self_(std::make_shared<WeakRef>(env, thiz)) {}
  ~Codec() {
    Release();
    if (window_ != nullptr) ANativeWindow_release(window_);
  }

  void Release() {
    // An admitted JNI call can still hold this Codec. Retire and wake its
    // blocking calls now, rather than deferring shutdown to the last owner.
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (released_) return;
      released_ = true;
      configured_ = false;
    }
    StopLocked();
    ANativeWindow* window;
    {
      std::lock_guard<std::mutex> lock(mu_);
      window = window_;
      window_ = nullptr;
      reader_.reset();
    }
    if (window != nullptr) ANativeWindow_release(window);
  }

  const CodecEntry& entry() const { return entry_; }

  bool Configure(JNIEnv* env, Format format, jobject surface, std::string* error,
                 int* status = nullptr) {
    if (status != nullptr) *status = android::BAD_VALUE;
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    std::lock_guard<std::mutex> lock(mu_);
    if (released_ || fatal_ || configured_) {
      *error = "configure() requires an idle codec; reset after a fatal error";
      if (status != nullptr) *status = android::INVALID_OPERATION;
      return false;
    }
    std::string mime = format.Mime();
    if (!mime.empty() && mime != entry_.mime) {
      *error = "format " + mime + " for " + entry_.name;
      return false;
    }
    if (mime.empty()) AMediaFormat_setString(format.get(), "mime", entry_.mime.c_str());
    if (entry_.encoder && !ConfigureEncoder(format, error)) return false;
    input_format_ = std::move(format);
    if (entry_.encoder) {
      BeginMetricsLocked();
      configured_ = true;
      return true;
    }
    SetSurfaceLocked(env, surface);
    int32_t max_input = 0;
    if (!input_format_.GetInt32("max-input-size", &max_input) || max_input <= 0) {
      int32_t w = 0, h = 0;
      input_format_.GetInt32("width", &w);
      input_format_.GetInt32("height", &h);
      max_input = entry_.video ? std::max(w * h * 3 / 2, 1 << 20) : 1 << 16;
    }
    for (Slot& s : inputs_) {
      s.mem.assign(static_cast<size_t>(max_input), 0);
      s.client = false;
    }
    for (Slot& s : outputs_) {
      s = Slot();
    }
    BeginMetricsLocked();
    configured_ = true;
    return true;
  }

  void SetSurface(JNIEnv* env, jobject surface, int* status = nullptr) {
    if (status != nullptr) *status = android::OK;
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    std::lock_guard<std::mutex> lock(mu_);
    if (released_ || fatal_) {
      if (status != nullptr) *status = android::INVALID_OPERATION;
      return;
    }
    SetSurfaceLocked(env, surface);
  }

  // With mu_ held: the output Surface: a native window, or an ImageReader's.
  void SetSurfaceLocked(JNIEnv* env, jobject surface) {
    if (window_ != nullptr) ANativeWindow_release(window_);
    window_ = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    reader_.reset();
    if (surface != nullptr && window_ == nullptr && IsReaderSurface(env, surface)) {
      reader_ = std::make_shared<GlobalRef>(env, surface);
    }
  }

  bool has_surface() const { return window_ != nullptr || reader_ != nullptr; }

  // --- Encoders: the input Surface, the end of its stream, parameters.

  jobject CreateInputSurface(JNIEnv* env, std::string* error) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    std::lock_guard<std::mutex> lock(mu_);
    if (!entry_.encoder || !entry_.video) {
      *error = "createInputSurface() is for video encoders";
      return nullptr;
    }
    if (released_ || !configured_ || started_) {
      *error = "createInputSurface() must be called after configure() and before start()";
      return nullptr;
    }
    if (input_surface_ == nullptr) {
      input_surface_.reset(gesher::media::CreateInputSurface(enc_params_.width, enc_params_.height));
      input_surface_->SetCallback([this](const uint32_t* rgba, int w, int h, int64_t ts_ns) {
        OnSurfaceFrame(rgba, w, h, ts_ns);
      });
    }
    return input_surface_->NewJavaSurface(env);
  }

  bool SignalEndOfInputStream(std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (released_ || input_surface_ == nullptr) {
      *error = "signalEndOfInputStream() without an input surface";
      return false;
    }
    surface_eos_ = true;
    cv_.notify_all();
    return true;
  }

  bool SetParameters(const Format& parameters) {
    int32_t bitrate = 0, sync = 0;
    parameters.GetInt32("video-bitrate", &bitrate);
    const bool request_sync = parameters.GetInt32("request-sync", &sync);
    std::lock_guard<std::mutex> lock(mu_);
    if (released_ || fatal_) return false;
    if (configured_ && entry_.video && entry_.encoder && !enc_eos_sent_) {
      if (request_sync) force_key_ = true;
      if (bitrate > 0) pending_bitrate_ = bitrate;
    }
    return true;
  }

  jobject GetMetrics(JNIEnv* env) {
    std::optional<CodecMetrics> metrics;
    bool released;
    {
      std::lock_guard<std::mutex> lock(mu_);
      released = released_;
      metrics = metrics_;
    }
    if (released) {
      jniThrow(env, "java/lang/IllegalStateException", "getMetrics() after release()");
      return nullptr;
    }
    return MakeMetricsBundle(env, entry_, metrics);
  }

  void SetAsync(bool async) {
    std::lock_guard<std::mutex> lock(mu_);
    if (released_) return;
    async_ = async;
  }

  void EnableFrameRendered(bool enable) { frame_rendered_ = enable; }

  bool Start(std::string* error) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    std::unique_lock<std::mutex> lock(mu_);
    if (released_ || fatal_ || !configured_) {
      *error = "start() before configure()";
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
    if (async_ && input_surface_ == nullptr) {
      for (int i = 0; i < kSlots; i++) {
        if (!inputs_[i].client) {
          inputs_[i].client = true;
          PostLocked(kCbInputAvailable, i, nullptr);
        }
      }
    }
    cv_.notify_all();
    return true;
  }

  void Stop(int* status = nullptr) {
    if (status != nullptr) *status = android::OK;
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (fatal_) {
        if (status != nullptr) *status = android::INVALID_OPERATION;
        return;
      }
    }
    StopLocked();
  }

  // Start, stop, reset and release serialize worker ownership with
  // lifecycle_mu_. Blocking dequeues/flushes never take that mutex.
  void StopLocked() {
    StopWorker();
    // Abandon waits for an in-flight producer callback, which takes mu_.
    if (input_surface_ != nullptr) input_surface_->Abandon();
    std::lock_guard<std::mutex> lock(mu_);
    input_surface_.reset();
    configured_ = false;
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
    for (Slot& s : inputs_) s.client = false;
    for (Slot& s : outputs_) {
      s.client = false;
      s.frame.reset();
    }
    audio_.reset();
    video_.reset();
    output_known_ = false;
    format_pending_ = false;
    config_.clear();
    pending_config_.clear();
    flush_requested_ = false;
    flushed_ = false;
  }

  void Reset() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    StopLocked();
    std::lock_guard<std::mutex> lock(mu_);
    // As with stop, getMetrics keeps the last session until a new configure.
    target_bitrate_.reset();
    configured_ = false;
    fatal_ = false;
    failed_ = false;
  }

  void Flush(int* status = nullptr) {
    if (status != nullptr) *status = android::OK;
    std::unique_lock<std::mutex> lock(mu_);
    if (released_ || fatal_ || !started_ || !worker_running_) {
      if (status != nullptr) *status = android::INVALID_OPERATION;
      return;
    }
    const uint64_t fatal_generation = fatal_generation_;
    const uint64_t generation = ++wait_generation_;
    // The worker finishes the sample it is decoding, then flushes.
    flush_requested_ = true;
    work_.clear();
    cv_.notify_all();
    done_cv_.notify_all();
    done_cv_.wait(lock, [this, generation] {
      return generation != wait_generation_ || quit_ || !worker_running_ || !flush_requested_;
    });
    if (generation != wait_generation_ || quit_ || !started_ || released_) {
      if (status != nullptr) {
        *status = fatal_generation != fatal_generation_ ? android::UNKNOWN_ERROR
                                                       : android::INVALID_OPERATION;
      }
      return;
    }
    ready_.clear();
    for (Slot& s : inputs_) s.client = false;
    for (Slot& s : outputs_) {
      s.client = false;
      s.frame.reset();
    }
    // Asynchronous codecs wait for start() again; synchronous ones go on.
    if (async_) flushed_ = true;
  }

  int DequeueInput(int64_t timeout_us, int* status = nullptr) {
    if (status != nullptr) *status = android::OK;
    std::unique_lock<std::mutex> lock(mu_);
    const uint64_t generation = wait_generation_;
    if (released_ || !started_ || async_ || input_surface_ != nullptr) {
      if (status != nullptr) *status = android::INVALID_OPERATION;
      return kInfoTryAgainLater;
    }
    auto free_slot = [this] {
      for (int i = 0; i < kSlots; i++) {
        if (!inputs_[i].client && !InWork(i)) return i;
      }
      return -1;
    };
    int i = -1;
    auto ready = [&] {
      return generation != wait_generation_ || quit_ || !started_ || (i = free_slot()) >= 0;
    };
    if (timeout_us < 0) {
      cv_.wait(lock, ready);
    } else {
      cv_.wait_for(lock, std::chrono::microseconds(timeout_us), ready);
    }
    if (generation != wait_generation_ || quit_ || !started_ || released_) {
      if (status != nullptr) *status = android::INVALID_OPERATION;
      return kInfoTryAgainLater;
    }
    if (i < 0) return kInfoTryAgainLater;
    inputs_[i].client = true;
    return i;
  }

  bool QueueInput(int index, int offset, int size, int64_t time_us, int flags, std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (released_ || !started_ || index < 0 || index >= kSlots || !inputs_[index].client) {
      *error = "invalid input buffer index " + std::to_string(index);
      return false;
    }
    Slot& s = inputs_[index];
    if (offset < 0 || size < 0 || static_cast<size_t>(offset) + static_cast<size_t>(size) > s.mem.size()) {
      *error = "input range outside the buffer";
      return false;
    }
    s.client = false;
    s.offset = offset;
    s.size = size;
    s.time_us = time_us;
    s.flags = flags;
    work_.push_back(index);
    cv_.notify_all();
    return true;
  }

  int DequeueOutput(JNIEnv* env, jobject info, int64_t timeout_us, int* status = nullptr) {
    if (status != nullptr) *status = android::OK;
    std::unique_lock<std::mutex> lock(mu_);
    const uint64_t generation = wait_generation_;
    if (released_ || !started_ || async_) {
      if (status != nullptr) *status = android::INVALID_OPERATION;
      return kInfoTryAgainLater;
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
      return kInfoTryAgainLater;
    }
    if (format_pending_) {
      format_pending_ = false;
      return kInfoOutputFormatChanged;
    }
    if (ready_.empty()) return kInfoTryAgainLater;
    int i = ready_.front();
    ready_.pop_front();
    Slot& s = outputs_[i];
    s.client = true;
    env->CallVoidMethod(info, gJ.buffer_info_set, 0, s.size, static_cast<jlong>(s.time_us), s.flags);
    return i;
  }

  bool ReleaseOutput(int index, bool render, bool has_timestamp, int64_t time_ns,
                     std::string* error, int* release_status = nullptr) {
    if (release_status != nullptr) *release_status = android::OK;
    std::shared_ptr<VideoFrame> frame;
    std::unique_ptr<ANativeWindow, decltype(&ANativeWindow_release)> window(
        nullptr, ANativeWindow_release);
    int64_t time_us = 0;
    uint64_t execution_generation = 0;
    std::shared_ptr<GlobalRef> reader;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (released_ || !started_) {
        *error = "releaseOutputBuffer() requires an executing codec";
        if (release_status != nullptr) *release_status = android::INVALID_OPERATION;
        return false;
      }
      if (index < 0 || index >= kSlots) {
        *error = "invalid output buffer index " + std::to_string(index);
        if (release_status != nullptr) *release_status = -ERANGE;
        return false;
      }
      if (!outputs_[index].client) {
        *error = "output buffer is not owned by the client";
        if (release_status != nullptr) *release_status = -EACCES;
        return false;
      }
      Slot& s = outputs_[index];
      s.client = false;
      frame = std::move(s.frame);
      time_us = s.time_us;
      execution_generation = execution_generation_;
      if (render && window_ != nullptr) {
        // SetSurface can release the codec's reference once mu_ is unlocked.
        // Hold a separate reference for the complete render operation.
        ANativeWindow_acquire(window_);
        window.reset(window_);
      }
      if (render) reader = reader_;
      cv_.notify_all();
    }
    if (frame == nullptr || (window == nullptr && reader == nullptr)) return true;
    // The boolean overload uses the presentation time. Explicit zero belongs
    // to the long overload and must survive delivery as zero.
    const int64_t timestamp_ns =
        has_timestamp ? time_ns : static_cast<int64_t>(static_cast<uint64_t>(time_us) * 1000);
    static gesher::perf::Site render_site("MediaCodec: render a frame into its Surface");
    const int64_t render_start = gesher::perf::Now();
    const int delivery_status = window != nullptr
        ? RenderFrame(window.get(), *frame, timestamp_ns)
        : DeliverToReader(reader->get(), *frame, timestamp_ns);
    if (window != nullptr) render_site.Add(gesher::perf::Now() - render_start);
    if (delivery_status == android::NO_INIT) {
      *error = "rendering to an abandoned output surface";
      RetireSurfaceFailure(execution_generation, *error);
      if (release_status != nullptr) *release_status = delivery_status;
      return false;
    }
    // AOSP consumes the output for other render failures, but does not report
    // that a frame was rendered. Obsolete consumers are the fatal exception.
    if (delivery_status == android::OK) {
      std::lock_guard<std::mutex> lock(mu_);
      if (execution_generation == execution_generation_ && started_ && !fatal_ && frame_rendered_) {
        PostFrameRendered(time_us);
      }
    }
    return true;
  }

  // AOSP's CCodec treats a disconnected output consumer as a fatal component
  // error. Retire the actual execution before reporting it. The failing JNI
  // caller owns its render reference separately, so surfaces can be detached
  // here without destroying an in-flight producer's window or reader.
  void RetireSurfaceFailure(uint64_t execution_generation, const std::string& error) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    bool notify_error;
    {
      std::lock_guard<std::mutex> lock(mu_);
      // A render admitted before stop/reset must not retire the new session.
      if (released_ || fatal_ || execution_generation != execution_generation_) return;
      fatal_ = true;
      ++fatal_generation_;
      configured_ = false;
      started_ = false;
      quit_ = true;
      // In FLUSHING the outstanding flush receives the fatal error directly;
      // STARTED/FLUSHED asynchronous operation receives onError instead.
      notify_error = async_ && !flush_requested_;
      async_ = false;
      ++wait_generation_;
      cv_.notify_all();
      done_cv_.notify_all();
    }
    // Only JNI release-output calls reach this helper; it never runs on the
    // decode worker. Join without mu_ before destroying its decoder state.
    StopLocked();
    if (input_surface_ != nullptr) {
      input_surface_->Abandon();
      input_surface_.reset();
    }
    ANativeWindow* window;
    {
      std::lock_guard<std::mutex> lock(mu_);
      window = window_;
      window_ = nullptr;
      reader_.reset();
      if (notify_error) PostFatalErrorLocked(error);
    }
    if (window != nullptr) ANativeWindow_release(window);
  }

  jobject InputBuffer(JNIEnv* env, int index) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!CheckInitializedLocked(env)) return nullptr;
    if (index < 0 || index >= kSlots) return nullptr;
    Slot& s = inputs_[index];
    if (s.mem.empty()) return nullptr;
    jobject b = NewByteBuffer(env, s.mem.data(), s.mem.size(), false);
    return b;
  }

  jobject OutputBuffer(JNIEnv* env, int index) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!CheckInitializedLocked(env)) return nullptr;
    if (index < 0 || index >= kSlots || has_surface()) return nullptr;
    Slot& s = outputs_[index];
    // A Surface can be removed while an output is held. Materialize that
    // frame only if a CPU accessor subsequently needs it.
    uint8_t* data = CpuOutputBytes(env, s);
    if (data == nullptr) return nullptr;
    const size_t capacity = s.mem.empty() ? OutputFrameBytes(*s.frame) : s.mem.size();
    jobject b = NewByteBuffer(env, data, capacity, true);
    if (b != nullptr) SetRange(env, b, 0, s.size);
    return b;
  }

  jobjectArray Buffers(JNIEnv* env, bool input) {
    jobjectArray a = env->NewObjectArray(kSlots, gJ.byte_buffer, nullptr);
    for (int i = 0; i < kSlots; i++) {
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

  // A video slot as a MediaCodec.MediaImage (YUV_420_888 over I420 planes): a
  // decoder's output (read only), or a raw video encoder's input (writable).
  jobject OutputImage(JNIEnv* env, int index, bool input = false) {
    int w, h;
    int64_t t;
    jobject buffer;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!CheckInitializedLocked(env)) return nullptr;
      if (!entry_.video || index < 0 || index >= kSlots) return nullptr;
      if (input) {
        if (!entry_.encoder || input_surface_ != nullptr) return nullptr;
        Slot& s = inputs_[index];
        w = enc_params_.width;
        h = enc_params_.height;
        const size_t size = static_cast<size_t>(w) * h + 2 * static_cast<size_t>((w + 1) / 2) * ((h + 1) / 2);
        if (s.mem.size() < size) return nullptr;
        t = 0;
        buffer = NewByteBuffer(env, s.mem.data(), size, false);
      } else {
        if (has_surface() || entry_.encoder) return nullptr;
        Slot& s = outputs_[index];
        if (s.size == 0) return nullptr;
        uint8_t* data = CpuOutputBytes(env, s);
        if (data == nullptr) return nullptr;
        w = out_width_;
        h = out_height_;
        t = s.time_us;
        buffer = NewByteBuffer(env, data, static_cast<size_t>(s.size), true);
      }
    }
    if (buffer == nullptr) return nullptr;
    // MediaImage2: type, planes, width, height, bit depth, allocated bit depth,
    // then Y, U, V, A: offset, column and row increments, subsampling.
    int32_t info[26] = {};
    const int cw = (w + 1) / 2;
    info[0] = 1;  // MEDIA_IMAGE_TYPE_YUV
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
    jobject image = env->NewObject(gJ.media_image, gJ.media_image_ctor, buffer, infoBuffer,
                                   input ? JNI_FALSE : JNI_TRUE, static_cast<jlong>(t * 1000), 0, 0, crop);
    env->DeleteLocalRef(buffer);
    env->DeleteLocalRef(infoBuffer);
    env->DeleteLocalRef(infoBytes);
    env->DeleteLocalRef(crop);
    return image;
  }

  jobject InputFormatMap(JNIEnv* env) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!CheckInitializedLocked(env)) return nullptr;
    return FormatToMap(env, input_format_);
  }

  jobject OutputFormatMap(JNIEnv* env) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!CheckInitializedLocked(env)) return nullptr;
    if (entry_.encoder) return FormatToMap(env, EncoderOutputFormatLocked());
    return OutputFormatLocked().Make(env);
  }

 private:
  bool CheckInitializedLocked(JNIEnv* env) const {
    if (!released_ && !fatal_) return true;
    jniThrow(env, "java/lang/IllegalStateException", "codec is released or requires reset after a fatal error");
    return false;
  }

  bool InWork(int index) const {
    return std::find(work_.begin(), work_.end(), index) != work_.end();
  }

  JavaMap OutputFormatLocked() const {
    JavaMap m;
    if (entry_.video) {
      m.strings["mime"] = "video/raw";
      m.ints["width"] = out_width_;
      m.ints["height"] = out_height_;
      m.ints["stride"] = out_width_;
      m.ints["slice-height"] = out_height_;
      m.ints["crop-left"] = 0;
      m.ints["crop-top"] = 0;
      m.ints["crop-right"] = out_width_ - 1;
      m.ints["crop-bottom"] = out_height_ - 1;
      m.ints["color-format"] = has_surface() ? kColorFormatSurface : kColorFormatYuv420Planar;
    } else {
      m.strings["mime"] = "audio/raw";
      m.ints["sample-rate"] = static_cast<int32_t>(pcm_.rate);
      m.ints["channel-count"] = static_cast<int32_t>(pcm_.channels);
      m.ints["pcm-encoding"] = out_float_ ? 4 : 2;  // ENCODING_PCM_FLOAT, ENCODING_PCM_16BIT
    }
    int64_t duration;
    if (input_format_.GetInt64("durationUs", &duration)) m.longs["durationUs"] = duration;
    return m;
  }

  void StopWorker() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      started_ = false;
      quit_ = true;
      ++execution_generation_;
      ++wait_generation_;
      cv_.notify_all();
      done_cv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mu_);
    worker_running_ = false;
    flush_requested_ = false;
    done_cv_.notify_all();
  }

  // With mu_ held: posts an event to Java (from any thread).
  void PostLocked(int what, int index, jobject obj) {
    JNIEnv* env = AttachedEnv("gesher-codec");
    if (env == nullptr) return;
    jobject self = env->NewLocalRef(self_->get());
    if (self == nullptr) return;
    env->CallVoidMethod(self, gJ.post_event, kEventCallback, what, index, obj);
    CheckException(env, "MediaCodec.postEventFromNative");
    env->DeleteLocalRef(self);
  }

  void PostFrameRendered(int64_t time_us) {
    JNIEnv* env = AttachedEnv("gesher-codec");
    if (env == nullptr) return;
    jobject self = env->NewLocalRef(self_->get());
    if (self == nullptr) return;
    JavaMap m;
    m.longs["0-media-time-us"] = time_us;
    m.longs["0-system-nano"] = gesher::MonotonicNanos();
    jobject map = m.Make(env);
    env->CallVoidMethod(self, gJ.post_event, kEventFrameRendered, 0, 0, map);
    CheckException(env, "MediaCodec.postEventFromNative");
    env->DeleteLocalRef(map);
    env->DeleteLocalRef(self);
  }

  // With mu_ held: the decoder, made at the first sample (codec config buffers
  // go into the format as csd-0, csd-1, ...).
  bool EnsureDecoder(std::string* error) {
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

  void Run() {
    if (entry_.encoder) {
      RunEncoder();
      return;
    }
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
      cv_.wait(lock, [this] { return quit_ || flush_requested_ || (started_ && !flushed_ && !work_.empty()); });
      if (quit_) return;
      if (flush_requested_) {
        if (audio_ != nullptr) audio_->Flush(0);
        if (video_ != nullptr) video_->Flush();
        flush_requested_ = false;
        done_cv_.notify_all();
        continue;
      }
      int index = work_.front();
      work_.pop_front();
      Slot& in = inputs_[index];
      Sample sample;
      sample.data.assign(in.mem.begin() + in.offset, in.mem.begin() + in.offset + in.size);
      sample.time_us = in.time_us;
      int flags = in.flags;
      // The slot is free again.
      if (async_ && !flushed_) {
        in.client = true;
        PostLocked(kCbInputAvailable, index, nullptr);
      }
      cv_.notify_all();
      if (flags & kFlagCodecConfig) {
        if (video_ != nullptr && !sample.data.empty()) {
          // Parameter sets mid-stream: in front of the next access unit.
          pending_config_.insert(pending_config_.end(), sample.data.begin(), sample.data.end());
        } else if (!sample.data.empty()) {
          config_.push_back(sample.data);
        }
        continue;
      }
      std::string error;
      if (!EnsureDecoder(&error)) {
        Fail(error);
        continue;
      }
      if (!pending_config_.empty()) {
        sample.data.insert(sample.data.begin(), pending_config_.begin(), pending_config_.end());
        pending_config_.clear();
      }
      bool eos = (flags & kFlagEndOfStream) != 0;
      // Decoding runs without the lock (the app may dequeue meanwhile).
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
        Fail(entry_.name + ": the decoder failed");
        continue;
      }
      if (video_ != nullptr) {
        for (VideoFrame& f : frames) {
          if (!EmitFrame(lock, std::make_shared<VideoFrame>(std::move(f)))) break;
        }
      } else if (!pcm.empty()) {
        EmitPcm(lock, pcm, sample.time_us);
      }
      if (eos) EmitEos(lock, sample.time_us);
    }
  }

  void Fail(const std::string& error) {
    MEDIA_LOGE("MediaCodec %s: %s", entry_.name.c_str(), error.c_str());
    failed_ = true;
    if (async_) {
      JNIEnv* env = AttachedEnv("gesher-codec");
      if (env == nullptr) return;
      jstring m = env->NewStringUTF(error.c_str());
      jobject e = env->NewObject(gJ.codec_exception, gJ.codec_exception_ctor, -1, 2, m);
      PostLocked(kCbError, 0, e);
      env->DeleteLocalRef(m);
      env->DeleteLocalRef(e);
    }
    cv_.notify_all();
  }

  // Only replace the previous session's metrics after successful configuration.
  // Stop, reset and worker failure retain them for getMetrics().
  void BeginMetricsLocked() {
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

  // With mu_ held, after the worker has stopped: CCodec reports UNKNOWN_ERROR
  // for this fatal component event; the release call itself returns NO_INIT.
  void PostFatalErrorLocked(const std::string& error) {
    JNIEnv* env = AttachedEnv("gesher-codec");
    if (env == nullptr) return;
    jstring message = env->NewStringUTF(error.c_str());
    if (message == nullptr) {
      CheckException(env, "MediaCodec fatal error message");
      return;
    }
    jobject exception = env->NewObject(gJ.codec_exception, gJ.codec_exception_ctor,
                                       android::UNKNOWN_ERROR, 0, message);
    if (exception != nullptr) PostLocked(kCbError, 0, exception);
    CheckException(env, "MediaCodec fatal error callback");
    env->DeleteLocalRef(exception);
    env->DeleteLocalRef(message);
  }

  // With the lock held: waits for a free output slot (-1 when stopping).
  int FreeOutput(std::unique_lock<std::mutex>& lock) {
    int found = -1;
    cv_.wait(lock, [&] {
      if (quit_ || flush_requested_) return true;
      for (int i = 0; i < kSlots; i++) {
        if (!outputs_[i].client && std::find(ready_.begin(), ready_.end(), i) == ready_.end()) {
          found = i;
          return true;
        }
      }
      return false;
    });
    return quit_ || flush_requested_ ? -1 : found;
  }

  // With the lock held: the output format is (now) known; announces a change.
  void OutputFormatIs(int width, int height) {
    if (output_known_ && (!entry_.video || (width == out_width_ && height == out_height_))) return;
    output_known_ = true;
    out_width_ = width;
    out_height_ = height;
    if (async_) {
      JNIEnv* env = AttachedEnv("gesher-codec");
      if (env == nullptr) return;
      env->PushLocalFrame(64);
      jobject map = OutputFormatLocked().Make(env);
      PostLocked(kCbOutputFormatChange, 0, map);
      env->PopLocalFrame(nullptr);
    } else {
      format_pending_ = true;
    }
    cv_.notify_all();
  }

  void Publish(int index) {
    if (async_) {
      Slot& s = outputs_[index];
      s.client = true;
      JNIEnv* env = AttachedEnv("gesher-codec");
      if (env == nullptr) return;
      // A local frame: the worker is a native thread, its local references never go away.
      env->PushLocalFrame(16);
      jobject info = env->NewObject(gJ.buffer_info, gJ.buffer_info_ctor);
      env->CallVoidMethod(info, gJ.buffer_info_set, 0, s.size, static_cast<jlong>(s.time_us), s.flags);
      PostLocked(kCbOutputAvailable, index, info);
      env->PopLocalFrame(nullptr);
    } else {
      ready_.push_back(index);
    }
    cv_.notify_all();
  }

  bool EmitFrame(std::unique_lock<std::mutex>& lock, std::shared_ptr<VideoFrame> f) {
    const int w = f->visible_width(), h = f->visible_height();
    metrics_->width = w;
    metrics_->height = h;
    OutputFormatIs(w, h);
    int i = FreeOutput(lock);
    if (i < 0) return false;
    Slot& s = outputs_[i];
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    const size_t size = static_cast<size_t>(w) * h + 2 * static_cast<size_t>(cw) * ch;
    s.size = static_cast<int>(size);
    s.offset = 0;
    s.time_us = f->time_us;
    s.flags = 0;
    s.frame = std::move(f);
    s.deferred_mem.reset();
    if (has_surface()) {
      // A reused slot may still own CPU storage from an earlier output mode.
      // Only free slots reach here, so no valid client buffer is invalidated.
      std::vector<uint8_t>().swap(s.mem);
    } else {
      CopyOutputFrameToBuffer(s);
    }
    Publish(i);
    return true;
  }

  void EmitPcm(std::unique_lock<std::mutex>& lock, const std::vector<uint8_t>& pcm, int64_t time_us) {
    metrics_->sample_rate = static_cast<int32_t>(pcm_.rate);
    metrics_->channel_count = static_cast<int32_t>(pcm_.channels);
    OutputFormatIs(0, 0);
    std::vector<uint8_t> converted;
    const std::vector<uint8_t>* data = &pcm;
    if (pcm_.format == PcmFormat::kFloat && !out_float_) {
      // Float from the decoder, 16-bit for the app (MediaCodec's default).
      size_t n = pcm.size() / 4;
      converted.resize(n * 2);
      const float* src = reinterpret_cast<const float*>(pcm.data());
      int16_t* dst = reinterpret_cast<int16_t*>(converted.data());
      for (size_t k = 0; k < n; k++) {
        float v = std::clamp(src[k], -1.0f, 1.0f);
        dst[k] = static_cast<int16_t>(v * 32767.0f);
      }
      data = &converted;
    }
    const size_t frame_bytes = std::max<size_t>(1, (out_float_ ? 4 : 2) * pcm_.channels);
    const size_t chunk = (64 * 1024 / frame_bytes) * frame_bytes;
    size_t done = 0;
    while (done < data->size()) {
      int i = FreeOutput(lock);
      if (i < 0) return;
      Slot& s = outputs_[i];
      size_t n = std::min(chunk, data->size() - done);
      if (s.mem.size() < chunk) s.mem.assign(chunk, 0);
      memcpy(s.mem.data(), data->data() + done, n);
      s.size = static_cast<int>(n);
      s.offset = 0;
      s.time_us = time_us + (pcm_.rate > 0 ? static_cast<int64_t>(done / frame_bytes) * 1000000 / pcm_.rate : 0);
      s.flags = 0;
      s.frame.reset();
      done += n;
      Publish(i);
    }
  }

  void EmitEos(std::unique_lock<std::mutex>& lock, int64_t time_us) {
    if (!output_known_ && !entry_.encoder) {
      OutputFormatIs(entry_.video ? std::max(out_width_, 1) : 0, std::max(out_height_, 1));
    }
    int i = FreeOutput(lock);
    if (i < 0) return;
    Slot& s = outputs_[i];
    if (s.mem.empty()) s.mem.assign(16, 0);
    s.size = 0;
    s.offset = 0;
    s.time_us = time_us;
    s.flags = kFlagEndOfStream;
    s.frame.reset();
    s.deferred_mem.reset();
    Publish(i);
  }

  // --- Encoders ---------------------------------------------------------------------

  // With mu_ held: the encoder's parameters from the format.
  bool ConfigureEncoder(const Format& format, std::string* error) {
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
        *error = entry_.name + ": the format has no width and height";
        return false;
      }
      if (VideoEncoder::Create(entry_.mime, p, error) == nullptr) return false;
      color_format_ = kColorFormatYuv420Flexible;
      format.GetInt32("color-format", &color_format_);
      enc_params_ = p;
    } else {
      AudioEncoderParams p;
      if (format.GetInt32("sample-rate", &v) && v > 0) p.sample_rate = static_cast<uint32_t>(v);
      if (format.GetInt32("channel-count", &v) && v > 0) p.channels = static_cast<uint32_t>(v);
      if (format.GetInt32("bitrate", &v) && v > 0) p.bitrate = v;
      // Preserve an explicit profile, including an invalid value, so the
      // encoder factory can reject it instead of silently selecting AAC-LC.
      if (format.GetInt32("aac-profile", &v)) p.aac_profile = v;
      auto encoder = AudioEncoder::Create(entry_.mime, p, error);
      if (encoder == nullptr) return false;
      p.bitrate = encoder->ConfiguredBitrate();
      int32_t encoding = 2;
      format.GetInt32("pcm-encoding", &encoding);
      in_float_ = encoding == 4;
      audio_params_ = p;
    }
    int32_t max_input = 0;
    if (!format.GetInt32("max-input-size", &max_input) || max_input <= 0) {
      max_input = entry_.video ? enc_params_.width * enc_params_.height * 3 / 2 + 64 : 1 << 16;
    }
    for (Slot& s : inputs_) {
      s.mem.assign(static_cast<size_t>(max_input), 0);
      s.client = false;
    }
    for (Slot& s : outputs_) s = Slot();
    return true;
  }

  // The input Surface's frames (the producer's thread): RGBA to I420, queued for the worker.
  void OnSurfaceFrame(const uint32_t* rgba, int w, int h, int64_t ts_ns) {
    auto f = std::make_shared<VideoFrame>();
    f->Allocate(enc_params_.width, enc_params_.height);
    RgbaToI420(rgba, w, h, f.get());
    if (ts_ns <= 0) {
      ts_ns = gesher::MonotonicNanos();
    }
    f->time_us = ts_ns / 1000;
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_ || surface_eos_) return;
    // A consumer that falls behind loses the oldest frames.
    while (surface_frames_.size() >= 8) surface_frames_.pop_front();
    surface_frames_.push_back(std::move(f));
    cv_.notify_all();
  }

  // With mu_ held: the format of what comes out.
  Format EncoderOutputFormatLocked() const {
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
      if (entry_.mime == "audio/mp4a-latm") AMediaFormat_setInt32(f.get(), "aac-profile", audio_params_.aac_profile);
    }
    for (size_t i = 0; i < csd_.size(); i++) {
      const std::string key = "csd-" + std::to_string(i);
      AMediaFormat_setBuffer(f.get(), key.c_str(), csd_[i].data(), csd_[i].size());
    }
    return f;
  }

  // With the lock held: the codec-specific data is known: the output format changes.
  void EncoderFormatIs(const EncodedPacket& config) {
    csd_.clear();
    if (entry_.mime == "video/avc") {
      // csd-0 the SPS, csd-1 the PPS, each with its start code (as Android's encoders give them).
      for (auto [nal, len] : SplitAnnexB(config.data.data(), config.data.size())) {
        if (len == 0) continue;
        std::vector<uint8_t> unit = {0, 0, 0, 1};
        unit.insert(unit.end(), nal, nal + len);
        const int type = nal[0] & 0x1f;
        if (type == 7) csd_.insert(csd_.begin(), unit);
        if (type == 8) csd_.push_back(unit);
      }
    } else if (entry_.mime == "audio/opus") {
      // OpusHead, then the codec delay and the seek pre-roll in ns (as MediaExtractor gives them).
      csd_.push_back(config.data);
      const int64_t delay_ns = audio_enc_ != nullptr && audio_params_.sample_rate > 0
                                   ? static_cast<int64_t>(audio_enc_->delay()) * 1000000000 / audio_params_.sample_rate
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
    if (async_) {
      JNIEnv* env = AttachedEnv("gesher-codec");
      if (env == nullptr) return;
      env->PushLocalFrame(64);
      jobject map = FormatToMap(env, EncoderOutputFormatLocked());
      PostLocked(kCbOutputFormatChange, 0, map);
      env->PopLocalFrame(nullptr);
    } else {
      format_pending_ = true;
    }
    cv_.notify_all();
  }

  // With the lock held: one packet into an output slot.
  bool EmitPacket(std::unique_lock<std::mutex>& lock, const EncodedPacket& pkt) {
    if (pkt.config) EncoderFormatIs(pkt);
    int i = FreeOutput(lock);
    if (i < 0) return false;
    Slot& s = outputs_[i];
    if (s.mem.size() < pkt.data.size()) s.mem.assign(std::max<size_t>(pkt.data.size(), 4096), 0);
    memcpy(s.mem.data(), pkt.data.data(), pkt.data.size());
    s.size = static_cast<int>(pkt.data.size());
    s.offset = 0;
    s.time_us = pkt.time_us;
    s.flags = (pkt.config ? kFlagCodecConfig : 0) | (pkt.key && !pkt.config ? kFlagKeyFrame : 0);
    s.frame.reset();
    Publish(i);
    return true;
  }

  // With mu_ held: the encoder, made at the first input.
  bool EnsureEncoder(std::string* error) {
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

  // A raw input slot's bytes as an I420 frame (planar or semi-planar input).
  bool InputFrame(const uint8_t* p, size_t n, int64_t time_us, VideoFrame* f) const {
    const int w = enc_params_.width, h = enc_params_.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    const size_t need = static_cast<size_t>(w) * h + 2 * static_cast<size_t>(cw) * ch;
    if (n < need) return false;
    f->Allocate(w, h);
    f->time_us = time_us;
    if (color_format_ == kColorFormatYuv420SemiPlanar) {
      Nv12ToI420(p, w, h, f);
    } else {
      memcpy(f->data.data(), p, need);
    }
    return true;
  }

  void RunEncoder() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
      cv_.wait(lock, [this] {
        return quit_ || flush_requested_ ||
               (started_ && !flushed_ && !enc_eos_sent_ &&
                (!work_.empty() || !surface_frames_.empty() || surface_eos_));
      });
      if (quit_) return;
      if (flush_requested_) {
        video_enc_.reset();
        audio_enc_.reset();
        surface_frames_.clear();
        pending_bitrate_.reset();
        force_key_ = false;
        flush_requested_ = false;
        done_cv_.notify_all();
        continue;
      }
      std::string error;
      if (!EnsureEncoder(&error)) {
        work_.clear();
        surface_frames_.clear();
        Fail(error);
        continue;
      }
      // Both input modes converge here. Only the worker may touch the backend
      // while it runs, and a rejected target does not interrupt encoding.
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
        Slot& in = inputs_[index];
        std::vector<uint8_t> bytes(in.mem.begin() + in.offset, in.mem.begin() + in.offset + in.size);
        const int64_t time_us = in.time_us;
        const int flags = in.flags;
        if (async_ && !flushed_) {
          in.client = true;
          PostLocked(kCbInputAvailable, index, nullptr);
        }
        cv_.notify_all();
        const bool key = force_key_;
        force_key_ = false;
        eos = (flags & kFlagEndOfStream) != 0;
        eos_time = time_us;
        lock.unlock();
        if (!bytes.empty()) {
          if (video_enc_ != nullptr) {
            VideoFrame f;
            ok = InputFrame(bytes.data(), bytes.size(), time_us, &f) && video_enc_->Encode(f, key, &packets);
          } else {
            std::vector<int16_t> pcm;
            if (in_float_) {
              const size_t count = bytes.size() / 4;
              pcm.resize(count);
              const float* fp = reinterpret_cast<const float*>(bytes.data());
              for (size_t k = 0; k < count; k++) {
                pcm[k] = static_cast<int16_t>(std::clamp(fp[k], -1.0f, 1.0f) * 32767.0f);
              }
            } else {
              pcm.resize(bytes.size() / 2);
              memcpy(pcm.data(), bytes.data(), pcm.size() * 2);
            }
            ok = audio_enc_->Encode(pcm.data(), pcm.size() / audio_params_.channels, time_us, &packets);
          }
        }
        if (ok && eos) ok = video_enc_ != nullptr ? video_enc_->Drain(&packets) : audio_enc_->Drain(&packets);
        lock.lock();
      } else if (surface_eos_) {
        eos = true;
        lock.unlock();
        ok = video_enc_->Drain(&packets);
        lock.lock();
      }
      if (quit_ || flush_requested_) continue;
      if (!ok) {
        Fail(entry_.name + ": the encoder failed");
        continue;
      }
      for (const EncodedPacket& pkt : packets) {
        if (!EmitPacket(lock, pkt)) break;
        last_out_us_ = std::max(last_out_us_, pkt.time_us);
      }
      if (eos) {
        EmitEos(lock, std::max(eos_time, last_out_us_));
        enc_eos_sent_ = true;
      }
    }
  }

  CodecEntry entry_;
  std::shared_ptr<WeakRef> self_;
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
  std::optional<CodecMetrics> metrics_;
  std::atomic<bool> frame_rendered_{false};
  Format input_format_;
  ANativeWindow* window_ = nullptr;
  Slot inputs_[kSlots];
  Slot outputs_[kSlots];
  std::deque<int> work_;   // queued inputs, in order
  std::deque<int> ready_;  // decoded outputs not yet dequeued (synchronous mode)
  std::vector<std::vector<uint8_t>> config_;
  std::vector<uint8_t> pending_config_;
  std::unique_ptr<AudioDecoder> audio_;
  std::unique_ptr<VideoDecoder> video_;
  PcmInfo pcm_;
  bool out_float_ = false;
  bool output_known_ = false, format_pending_ = false;
  int out_width_ = 0, out_height_ = 0;
  // An ImageReader's Surface as the output (decoders).
  std::shared_ptr<GlobalRef> reader_;
  // Encoders.
  VideoEncoderParams enc_params_;
  AudioEncoderParams audio_params_;
  int color_format_ = kColorFormatYuv420Flexible;
  bool in_float_ = false;
  std::unique_ptr<VideoEncoder> video_enc_;
  std::unique_ptr<AudioEncoder> audio_enc_;
  std::unique_ptr<InputSurface> input_surface_;
  std::deque<std::shared_ptr<VideoFrame>> surface_frames_;
  bool surface_eos_ = false, enc_eos_sent_ = false, force_key_ = false;
  std::optional<int> target_bitrate_;
  std::optional<int> pending_bitrate_;
  std::vector<std::vector<uint8_t>> csd_;
  int64_t last_out_us_ = 0;
};

// Protect the Java field and its heap holder only. Never stop a worker, wait
// for a callback, or invoke Java while holding this context mutex.
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
    // Buffer's own (older class files return Buffer).
    env->ExceptionClear();
    gJ.position = env->GetMethodID(gJ.byte_buffer, "position", "(I)Ljava/nio/Buffer;");
    gJ.limit = env->GetMethodID(gJ.byte_buffer, "limit", "(I)Ljava/nio/Buffer;");
    gJ.clear = env->GetMethodID(gJ.byte_buffer, "clear", "()Ljava/nio/Buffer;");
  }
  jclass bo = env->FindClass("java/nio/ByteOrder");
  jobject native_order = env->CallStaticObjectMethod(bo, env->GetStaticMethodID(bo, "nativeOrder", "()Ljava/nio/ByteOrder;"));
  gJ.native_order = env->NewGlobalRef(native_order);
  gJ.hash_map = global("java/util/HashMap");
  gJ.hash_map_ctor = env->GetMethodID(gJ.hash_map, "<init>", "()V");
  gJ.hash_map_put = env->GetMethodID(gJ.hash_map, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
  gJ.persistable_bundle = global("android/os/PersistableBundle");
  if (gJ.persistable_bundle != nullptr) {
    gJ.persistable_bundle_ctor = env->GetMethodID(gJ.persistable_bundle, "<init>", "()V");
    gJ.bundle_put_int = env->GetMethodID(gJ.persistable_bundle, "putInt", "(Ljava/lang/String;I)V");
    gJ.bundle_put_string = env->GetMethodID(gJ.persistable_bundle, "putString",
                                           "(Ljava/lang/String;Ljava/lang/String;)V");
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
  const CodecEntry* entry = name_is_type ? FindByType(n, encoder) : FindByName(n);
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
  if (auto c = GetCodec(env, thiz)) c->SetAsync(cb != nullptr);
}

void JNICALL MC_configure(JNIEnv* env, jobject thiz, jobjectArray keys, jobjectArray values,
                          jobject surface, jobject crypto, jobject, jint flags) {
  auto c = GetCodec(env, thiz);
  if (c == nullptr) return;
  if (crypto != nullptr) {
    ThrowCodecException(env, -1, "MediaCrypto (DRM) is not supported on Gesher");
    return;
  }
  if (((flags & 1 /* CONFIGURE_FLAG_ENCODE */) != 0) != c->entry().encoder) {
    jniThrow(env, "java/lang/IllegalArgumentException",
             c->entry().encoder ? "an encoder needs CONFIGURE_FLAG_ENCODE" : "a decoder cannot encode");
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
  if (!c->SignalEndOfInputStream(&error)) jniThrow(env, "java/lang/IllegalStateException", error.c_str());
}

jobject JNICALL MC_createInputSurface(JNIEnv* env, jobject thiz) {
  auto c = GetCodec(env, thiz);
  if (c == nullptr) return nullptr;
  std::string error;
  jobject s = c->CreateInputSurface(env, &error);
  if (s == nullptr && !env->ExceptionCheck()) jniThrow(env, "java/lang/IllegalStateException", error.c_str());
  return s;
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
  if (!input) return c->OutputBuffer(env, index);
  jobject b = c->InputBuffer(env, index);
  return b;
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
void JNICALL MC_setVideoScalingMode(JNIEnv*, jobject, jint) {}

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
    {"getFormatNative", "(Z)Ljava/util/Map;", GESHER_NATIVE(MC_getFormatNative)},
    {"getOutputFormatNative", "(I)Ljava/util/Map;", GESHER_NATIVE(MC_getOutputFormatNative)},
    {"getBuffers", "(Z)[Ljava/nio/ByteBuffer;", GESHER_NATIVE(MC_getBuffers)},
    {"getBuffer", "(ZI)Ljava/nio/ByteBuffer;", GESHER_NATIVE(MC_getBuffer)},
    {"getImage", "(ZI)Landroid/media/Image;", GESHER_NATIVE(MC_getImage)},
    {"getCanonicalName", "()Ljava/lang/String;", GESHER_NATIVE(MC_getCanonicalName)},
    {"setParameters", "([Ljava/lang/String;[Ljava/lang/Object;)V", GESHER_NATIVE(MC_setParameters)},
    {"setVideoScalingMode", "(I)V", GESHER_NATIVE(MC_setVideoScalingMode)},
    {"native_enableOnFrameRenderedListener", "(Z)V", GESHER_NATIVE(MC_enableOnFrameRenderedListener)},
    {"native_getMetrics", "()Landroid/os/PersistableBundle;", GESHER_NATIVE(MC_getMetrics)},
};

// --- MediaCodecList ----------------------------------------------------------------------

const CodecEntry* EntryAt(JNIEnv* env, jint index) {
  if (index < 0 || static_cast<size_t>(index) >= Codecs().size()) {
    jniThrow(env, "java/lang/IllegalArgumentException", nullptr);
    return nullptr;
  }
  return &Codecs()[static_cast<size_t>(index)];
}

jint JNICALL MCL_getCodecCount(JNIEnv*, jclass) { return static_cast<jint>(Codecs().size()); }

jstring JNICALL MCL_getCodecName(JNIEnv* env, jclass, jint index) {
  const CodecEntry* e = EntryAt(env, index);
  return e != nullptr ? env->NewStringUTF(e->name.c_str()) : nullptr;
}

jint JNICALL MCL_getAttributes(JNIEnv* env, jclass, jint index) {
  const CodecEntry* e = EntryAt(env, index);
  if (e == nullptr) return 0;
  // FLAG_IS_ENCODER (1); FLAG_IS_HARDWARE_ACCELERATED (8) or FLAG_IS_SOFTWARE_ONLY (4).
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
  for (size_t i = 0; i < Codecs().size(); i++) {
    if (Codecs()[i].name == n) return static_cast<jint>(i);
  }
  return -2;  // -ENOENT
}

// Profiles and levels by codec role, as on a device's codec list.
std::vector<std::pair<int, int>> ProfileLevels(const std::string& mime, bool encoder = false) {
  if (mime == "video/avc") return {{0x01, 0x8000}, {0x02, 0x8000}, {0x08, 0x8000}, {0x10000, 0x8000}, {0x80000, 0x8000}};
  if (mime == "video/hevc") return {{0x01, 0x10000}};
  if (mime == "video/x-vnd.on2.vp8") return {{0x01, 0x01}};
  if (mime == "video/x-vnd.on2.vp9") return {{0x01, 0x200}};
  if (mime == "video/av01") return {{0x1, 0x2000}};
  if (mime == "video/mp4v-es") return {{0x01, 0x80}};
  if (mime == "video/3gpp") return {{0x01, 0x80}};
  if (mime == "audio/mp4a-latm") {
    if (encoder) return {{2, 0}};  // The implemented AAC encoder emits LC only.
    return {{2, 0}, {5, 0}, {29, 0}};
  }
  return {};
}

jobject JNICALL MCL_getCodecCapabilities(JNIEnv* env, jclass, jint index, jstring type) {
  const CodecEntry* e = EntryAt(env, index);
  if (e == nullptr) return nullptr;
  if (type == nullptr || Utf8(env, type) != e->mime) {
    jniThrow(env, "java/lang/IllegalArgumentException", nullptr);
    return nullptr;
  }
  jclass pl_class = env->FindClass("android/media/MediaCodecInfo$CodecProfileLevel");
  auto pls = ProfileLevels(e->mime, e->encoder);
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
  if (e->video) colors = {kColorFormatYuv420Flexible, kColorFormatYuv420Planar, kColorFormatSurface};
  if (e->video && e->encoder) colors.push_back(kColorFormatYuv420SemiPlanar);
  jintArray color_array = env->NewIntArray(static_cast<jsize>(colors.size()));
  if (!colors.empty()) env->SetIntArrayRegion(color_array, 0, static_cast<jsize>(colors.size()), colors.data());

  JavaMap defaults;
  defaults.strings["mime"] = e->mime;
  JavaMap details;
  if (e->video) {
    const int mw = e->max_width > 0 ? e->max_width : 1920;
    const int mh = e->max_height > 0 ? e->max_height : 1088;
    details.strings["size-range"] = "2x2-" + std::to_string(mw) + "x" + std::to_string(mh);
    details.strings["alignment"] = "2x2";
    // The codec's own block size (MediaCodecInfo scales the block counts by its
    // ratio to the level limits' blocks: 8x8 for H.265, VP9 and AV1).
    const bool small_blocks = e->mime == "video/hevc" || e->mime == "video/x-vnd.on2.vp9" ||
                              e->mime == "video/av01";
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
    details.strings["bitrate-range"] = !e->encoder ? "8000-960000"
        : e->mime == "audio/opus" ? "6000-510000" : "96000-192000";
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
    impl = env->NewObject(impl_class, ctor, pl_array, color_array, e->encoder ? JNI_TRUE : JNI_FALSE, default_map, details_map);
    if (env->ExceptionCheck()) {
      // Not an abort in the FindClass below: say why, and give no capabilities.
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
    jmethodID ctor = env->GetMethodID(caps_class, "<init>",
                                      "(Landroid/media/MediaCodecInfo$CodecCapabilities$CodecCapsIntf;)V");
    caps = env->NewObject(caps_class, ctor, impl);
  } else {
    // Older framework: the constructor takes the arrays and maps itself.
    env->ExceptionClear();
    jmethodID ctor = env->GetMethodID(caps_class, "<init>",
                                      "([Landroid/media/MediaCodecInfo$CodecProfileLevel;[IZ"
                                      "Ljava/util/Map;Ljava/util/Map;)V");
    caps = env->NewObject(caps_class, ctor, pl_array, color_array, e->encoder ? JNI_TRUE : JNI_FALSE, default_map, details_map);
  }
  CheckException(env, "MediaCodecList.getCodecCapabilities");
  env->DeleteLocalRef(default_map);
  env->DeleteLocalRef(details_map);
  env->DeleteLocalRef(pl_array);
  env->DeleteLocalRef(color_array);
  if (impl != nullptr) env->DeleteLocalRef(impl);
  return caps;
}

jobject JNICALL MCL_getGlobalSettings(JNIEnv* env, jclass) { return JavaMap().Make(env); }

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
  // The classes and members MediaCodecList uses too (it may run before MediaCodec's
  // native_init): after the registration, as GetFieldID initializes MediaCodec, whose
  // static initializer calls native_init.
  if (jclass mc = env->FindClass("android/media/MediaCodec")) {
    MC_init(env, mc);
    env->DeleteLocalRef(mc);
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  return 0;
}

}  // namespace gesher::media
