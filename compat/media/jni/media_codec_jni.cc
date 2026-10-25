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
// the app releases the buffer with render = true. Encoders are not
// available (MediaCodecList lists decoders only).

#include <jni.h>

#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../audio/log.h"
#include "../player/decoder.h"
#include "../player/extractor.h"
#include "../video/video_decoder.h"
#include "jni_util.h"
#include "media_jni.h"

// The ANativeWindow functions dalvikvm defines (docs/graphics.md) and
// libandroid_runtime's host ANativeWindow_fromSurface.
struct ANativeWindow;
struct ARect {
  int32_t left, top, right, bottom;
};
struct ANativeWindow_Buffer {
  int32_t width, height, stride, format;
  void* bits;
  uint32_t reserved[6];
};
extern "C" {
ANativeWindow* ANativeWindow_fromSurface(JNIEnv* env, jobject surface);
void ANativeWindow_release(ANativeWindow* window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow* window, int32_t w, int32_t h, int32_t format);
int32_t ANativeWindow_lock(ANativeWindow* window, ANativeWindow_Buffer* buffer, ARect* bounds);
int32_t ANativeWindow_unlockAndPost(ANativeWindow* window);
}

namespace gesher::media {
namespace {

// MediaCodec's constants.
constexpr int kInfoTryAgainLater = -1;
constexpr int kInfoOutputFormatChanged = -2;
constexpr int kFlagCodecConfig = 2;
constexpr int kFlagEndOfStream = 4;
constexpr int kEventCallback = 1;
constexpr int kEventFrameRendered = 3;
constexpr int kCbInputAvailable = 1;
constexpr int kCbOutputAvailable = 2;
constexpr int kCbError = 3;
constexpr int kCbOutputFormatChange = 4;
constexpr int kColorFormatYuv420Planar = 19;
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

const CodecEntry* FindByType(const std::string& mime) {
  for (const CodecEntry& c : Codecs()) {
    if (c.mime == mime) return &c;
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
} gJ;

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
      env->NewObject(gJ.codec_exception, gJ.codec_exception_ctor, error, 2 /* ACTION_CODE_FATAL */, m));
  env->DeleteLocalRef(m);
  if (e != nullptr) env->Throw(e);
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

// --- A codec -----------------------------------------------------------------------------

struct Slot {
  std::vector<uint8_t> mem;
  bool client = false;  // dequeued by the app
  // Inputs: what was queued. Outputs: what is in it.
  int offset = 0, size = 0;
  int64_t time_us = 0;
  int flags = 0;
  std::shared_ptr<VideoFrame> frame;  // video outputs
};

class Codec {
 public:
  Codec(JNIEnv* env, jobject thiz, const CodecEntry& entry)
      : entry_(entry), self_(std::make_shared<WeakRef>(env, thiz)) {}
  ~Codec() {
    StopWorker();
    if (window_ != nullptr) ANativeWindow_release(window_);
  }

  const CodecEntry& entry() const { return entry_; }

  bool Configure(JNIEnv* env, Format format, jobject surface, std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string mime = format.Mime();
    if (!mime.empty() && mime != entry_.mime) {
      *error = "format " + mime + " for " + entry_.name;
      return false;
    }
    if (mime.empty()) AMediaFormat_setString(format.get(), "mime", entry_.mime.c_str());
    input_format_ = std::move(format);
    if (window_ != nullptr) ANativeWindow_release(window_);
    window_ = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
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
    configured_ = true;
    return true;
  }

  void SetSurface(JNIEnv* env, jobject surface) {
    std::lock_guard<std::mutex> lock(mu_);
    if (window_ != nullptr) ANativeWindow_release(window_);
    window_ = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
  }

  void SetAsync(bool async) {
    std::lock_guard<std::mutex> lock(mu_);
    async_ = async;
  }

  void EnableFrameRendered(bool enable) { frame_rendered_ = enable; }

  bool Start(std::string* error) {
    std::unique_lock<std::mutex> lock(mu_);
    if (!configured_) {
      *error = "start() before configure()";
      return false;
    }
    if (started_ && !flushed_) return true;
    flushed_ = false;
    started_ = true;
    if (!worker_.joinable()) {
      quit_ = false;
      worker_ = std::thread([this] { Run(); });
    }
    if (async_) {
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

  void Stop() {
    StopWorker();
    std::lock_guard<std::mutex> lock(mu_);
    started_ = false;
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
  }

  void Reset() {
    Stop();
    std::lock_guard<std::mutex> lock(mu_);
    configured_ = false;
  }

  void Flush() {
    std::unique_lock<std::mutex> lock(mu_);
    // The worker finishes the sample it is decoding, then flushes.
    flush_requested_ = true;
    work_.clear();
    cv_.notify_all();
    done_cv_.wait(lock, [this] { return !flush_requested_ || !worker_.joinable(); });
    ready_.clear();
    for (Slot& s : inputs_) s.client = false;
    for (Slot& s : outputs_) {
      s.client = false;
      s.frame.reset();
    }
    // Asynchronous codecs wait for start() again; synchronous ones go on.
    if (async_) flushed_ = true;
  }

  int DequeueInput(int64_t timeout_us) {
    std::unique_lock<std::mutex> lock(mu_);
    auto free_slot = [this] {
      for (int i = 0; i < kSlots; i++) {
        if (!inputs_[i].client && !InWork(i)) return i;
      }
      return -1;
    };
    int i = -1;
    auto ready = [&] { return quit_ || !started_ || (i = free_slot()) >= 0; };
    if (timeout_us < 0) {
      cv_.wait(lock, ready);
    } else {
      cv_.wait_for(lock, std::chrono::microseconds(timeout_us), ready);
    }
    if (i < 0) return kInfoTryAgainLater;
    inputs_[i].client = true;
    return i;
  }

  bool QueueInput(int index, int offset, int size, int64_t time_us, int flags, std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (index < 0 || index >= kSlots || !inputs_[index].client) {
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

  int DequeueOutput(JNIEnv* env, jobject info, int64_t timeout_us) {
    std::unique_lock<std::mutex> lock(mu_);
    auto ready = [this] { return quit_ || !started_ || format_pending_ || !ready_.empty() || failed_; };
    if (timeout_us < 0) {
      cv_.wait(lock, ready);
    } else {
      cv_.wait_for(lock, std::chrono::microseconds(timeout_us), ready);
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

  bool ReleaseOutput(int index, bool render, int64_t /*time_ns*/, std::string* error) {
    std::shared_ptr<VideoFrame> frame;
    ANativeWindow* window = nullptr;
    int64_t time_us = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (index < 0 || index >= kSlots || !outputs_[index].client) {
        *error = "invalid output buffer index " + std::to_string(index);
        return false;
      }
      Slot& s = outputs_[index];
      s.client = false;
      frame = std::move(s.frame);
      time_us = s.time_us;
      window = render ? window_ : nullptr;
      cv_.notify_all();
    }
    if (window != nullptr && frame != nullptr) {
      RenderFrame(window, *frame);
      if (frame_rendered_) PostFrameRendered(time_us);
    }
    return true;
  }

  jobject InputBuffer(JNIEnv* env, int index) {
    std::lock_guard<std::mutex> lock(mu_);
    if (index < 0 || index >= kSlots) return nullptr;
    Slot& s = inputs_[index];
    if (s.mem.empty()) return nullptr;
    jobject b = NewByteBuffer(env, s.mem.data(), s.mem.size(), false);
    return b;
  }

  jobject OutputBuffer(JNIEnv* env, int index) {
    std::lock_guard<std::mutex> lock(mu_);
    if (index < 0 || index >= kSlots || window_ != nullptr) return nullptr;
    Slot& s = outputs_[index];
    if (s.mem.empty()) return nullptr;
    jobject b = NewByteBuffer(env, s.mem.data(), s.mem.size(), true);
    if (b != nullptr) SetRange(env, b, 0, s.size);
    return b;
  }

  jobjectArray Buffers(JNIEnv* env, bool input) {
    jobjectArray a = env->NewObjectArray(kSlots, gJ.byte_buffer, nullptr);
    for (int i = 0; i < kSlots; i++) {
      jobject b = input ? InputBuffer(env, i) : OutputBuffer(env, i);
      if (b == nullptr) continue;
      env->SetObjectArrayElement(a, i, b);
      env->DeleteLocalRef(b);
    }
    return a;
  }

  // A video output slot as a MediaCodec.MediaImage (YUV_420_888 over the I420 planes).
  jobject OutputImage(JNIEnv* env, int index) {
    int w, h;
    int64_t t;
    jobject buffer;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!entry_.video || index < 0 || index >= kSlots || window_ != nullptr) return nullptr;
      Slot& s = outputs_[index];
      if (s.mem.empty() || s.size == 0) return nullptr;
      w = out_width_;
      h = out_height_;
      t = s.time_us;
      buffer = NewByteBuffer(env, s.mem.data(), static_cast<size_t>(s.size), true);
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
    jobject image = env->NewObject(gJ.media_image, gJ.media_image_ctor, buffer, infoBuffer, JNI_TRUE,
                                   static_cast<jlong>(t * 1000), 0, 0, crop);
    env->DeleteLocalRef(buffer);
    env->DeleteLocalRef(infoBuffer);
    env->DeleteLocalRef(infoBytes);
    env->DeleteLocalRef(crop);
    return image;
  }

  jobject InputFormatMap(JNIEnv* env) {
    std::lock_guard<std::mutex> lock(mu_);
    return FormatToMap(env, input_format_);
  }

  jobject OutputFormatMap(JNIEnv* env) {
    std::lock_guard<std::mutex> lock(mu_);
    return OutputFormatLocked().Make(env);
  }

 private:
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
      m.ints["color-format"] = window_ != nullptr ? kColorFormatSurface : kColorFormatYuv420Planar;
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
      quit_ = true;
      cv_.notify_all();
      done_cv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mu_);
    quit_ = false;
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
    m.longs["0-system-nano"] = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
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
        ok = video_ != nullptr ? video_->Decode(sample, &frames) : audio_->Decode(sample, &pcm);
      }
      if (ok && eos) {
        if (video_ != nullptr) {
          video_->Drain(&frames);
        } else {
          audio_->Drain(&pcm);
        }
      }
      lock.lock();
      if (!ok) {
        Fail(entry_.name + ": the decoder failed");
        continue;
      }
      if (quit_ || flush_requested_) continue;
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
    OutputFormatIs(w, h);
    int i = FreeOutput(lock);
    if (i < 0) return false;
    Slot& s = outputs_[i];
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    const size_t size = static_cast<size_t>(w) * h + 2 * static_cast<size_t>(cw) * ch;
    if (s.mem.size() < size) s.mem.assign(size, 0);
    // The visible part, tightly packed I420.
    uint8_t* d = s.mem.data();
    for (int y = 0; y < h; y++) {
      memcpy(d + static_cast<size_t>(y) * w,
             f->y() + static_cast<size_t>(f->crop_top + y) * f->stride_y + f->crop_left, static_cast<size_t>(w));
    }
    uint8_t* du = d + static_cast<size_t>(w) * h;
    uint8_t* dv = du + static_cast<size_t>(cw) * ch;
    for (int y = 0; y < ch; y++) {
      size_t row = static_cast<size_t>(f->crop_top / 2 + y) * f->stride_uv + f->crop_left / 2;
      memcpy(du + static_cast<size_t>(y) * cw, f->u() + row, static_cast<size_t>(cw));
      memcpy(dv + static_cast<size_t>(y) * cw, f->v() + row, static_cast<size_t>(cw));
    }
    s.size = static_cast<int>(size);
    s.offset = 0;
    s.time_us = f->time_us;
    s.flags = 0;
    s.frame = std::move(f);
    Publish(i);
    return true;
  }

  void EmitPcm(std::unique_lock<std::mutex>& lock, const std::vector<uint8_t>& pcm, int64_t time_us) {
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
    if (!output_known_) OutputFormatIs(entry_.video ? std::max(out_width_, 1) : 0, std::max(out_height_, 1));
    int i = FreeOutput(lock);
    if (i < 0) return;
    Slot& s = outputs_[i];
    if (s.mem.empty()) s.mem.assign(16, 0);
    s.size = 0;
    s.offset = 0;
    s.time_us = time_us;
    s.flags = kFlagEndOfStream;
    s.frame.reset();
    Publish(i);
  }

  CodecEntry entry_;
  std::shared_ptr<WeakRef> self_;
  std::mutex mu_;
  std::condition_variable cv_, done_cv_;
  std::thread worker_;
  bool quit_ = false;
  bool configured_ = false, started_ = false, flushed_ = false, async_ = false;
  bool flush_requested_ = false;
  bool failed_ = false;
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
};

Codec* GetCodec(JNIEnv* env, jobject thiz, bool throw_if_null = true) {
  auto* c = reinterpret_cast<Codec*>(env->GetLongField(thiz, gJ.context));
  if (c == nullptr && throw_if_null) jniThrow(env, "java/lang/IllegalStateException", nullptr);
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
  CheckException(env, "MediaCodec.native_init");
}

void JNICALL MC_setup(JNIEnv* env, jobject thiz, jstring name, jboolean name_is_type, jboolean encoder,
                      jint, jint) {
  if (name == nullptr) {
    jniThrow(env, "java/lang/NullPointerException", "No codec name specified");
    return;
  }
  std::string n = Utf8(env, name);
  if (encoder) {
    jniThrow(env, "java/io/IOException", ("Failed to find matching codec " + n +
                                           ": Gesher has no encoders yet").c_str());
    return;
  }
  const CodecEntry* entry = name_is_type ? FindByType(n) : FindByName(n);
  if (entry == nullptr) {
    if (name_is_type) {
      jniThrow(env, "java/io/IOException", ("Failed to find matching codec " + n).c_str());
    } else {
      jniThrow(env, "java/lang/IllegalArgumentException",
               ("Failed to initialize " + n + ", error 0xffffffc8 (NAME_NOT_FOUND)").c_str());
    }
    return;
  }
  env->SetLongField(thiz, gJ.context, reinterpret_cast<jlong>(new Codec(env, thiz, *entry)));
}

void JNICALL MC_release(JNIEnv* env, jobject thiz) {
  auto* c = GetCodec(env, thiz, false);
  env->SetLongField(thiz, gJ.context, 0);
  delete c;
}

void JNICALL MC_reset(JNIEnv* env, jobject thiz) {
  if (Codec* c = GetCodec(env, thiz)) c->Reset();
}

void JNICALL MC_setCallback(JNIEnv* env, jobject thiz, jobject cb) {
  if (Codec* c = GetCodec(env, thiz)) c->SetAsync(cb != nullptr);
}

void JNICALL MC_configure(JNIEnv* env, jobject thiz, jobjectArray keys, jobjectArray values,
                          jobject surface, jobject crypto, jobject, jint flags) {
  Codec* c = GetCodec(env, thiz);
  if (c == nullptr) return;
  if (crypto != nullptr) {
    ThrowCodecException(env, -1, "MediaCrypto (DRM) is not supported on Gesher");
    return;
  }
  if (flags & 1 /* CONFIGURE_FLAG_ENCODE */) {
    jniThrow(env, "java/lang/IllegalArgumentException", "Gesher has no encoders yet");
    return;
  }
  std::string error;
  if (!c->Configure(env, FormatFromJava(env, keys, values), surface, &error)) {
    jniThrow(env, "java/lang/IllegalArgumentException", error.c_str());
  }
}

void JNICALL MC_setSurface(JNIEnv* env, jobject thiz, jobject surface) {
  if (Codec* c = GetCodec(env, thiz)) c->SetSurface(env, surface);
}

void JNICALL MC_start(JNIEnv* env, jobject thiz) {
  Codec* c = GetCodec(env, thiz);
  if (c == nullptr) return;
  std::string error;
  if (!c->Start(&error)) jniThrow(env, "java/lang/IllegalStateException", error.c_str());
}

void JNICALL MC_stop(JNIEnv* env, jobject thiz) {
  if (Codec* c = GetCodec(env, thiz)) c->Stop();
}

void JNICALL MC_flush(JNIEnv* env, jobject thiz) {
  if (Codec* c = GetCodec(env, thiz)) c->Flush();
}

void JNICALL MC_queueInputBuffer(JNIEnv* env, jobject thiz, jint index, jint offset, jint size,
                                 jlong time_us, jint flags) {
  Codec* c = GetCodec(env, thiz);
  if (c == nullptr) return;
  std::string error;
  if (!c->QueueInput(index, offset, size, time_us, flags, &error)) {
    jniThrow(env, "java/lang/IllegalStateException", error.c_str());
  }
}

jint JNICALL MC_dequeueInputBuffer(JNIEnv* env, jobject thiz, jlong timeout_us) {
  Codec* c = GetCodec(env, thiz);
  return c != nullptr ? c->DequeueInput(timeout_us) : kInfoTryAgainLater;
}

jint JNICALL MC_dequeueOutputBuffer(JNIEnv* env, jobject thiz, jobject info, jlong timeout_us) {
  Codec* c = GetCodec(env, thiz);
  return c != nullptr ? c->DequeueOutput(env, info, timeout_us) : kInfoTryAgainLater;
}

void JNICALL MC_releaseOutputBuffer(JNIEnv* env, jobject thiz, jint index, jboolean render,
                                    jboolean, jlong time_ns) {
  Codec* c = GetCodec(env, thiz);
  if (c == nullptr) return;
  std::string error;
  if (!c->ReleaseOutput(index, render, time_ns, &error)) {
    jniThrow(env, "java/lang/IllegalStateException", error.c_str());
  }
}

void JNICALL MC_signalEndOfInputStream(JNIEnv* env, jobject) {
  jniThrow(env, "java/lang/IllegalStateException", "no input surface");
}

jobject JNICALL MC_getFormatNative(JNIEnv* env, jobject thiz, jboolean input) {
  Codec* c = GetCodec(env, thiz);
  if (c == nullptr) return nullptr;
  return input ? c->InputFormatMap(env) : c->OutputFormatMap(env);
}

jobject JNICALL MC_getOutputFormatNative(JNIEnv* env, jobject thiz, jint) {
  Codec* c = GetCodec(env, thiz);
  return c != nullptr ? c->OutputFormatMap(env) : nullptr;
}

jobjectArray JNICALL MC_getBuffers(JNIEnv* env, jobject thiz, jboolean input) {
  Codec* c = GetCodec(env, thiz);
  return c != nullptr ? c->Buffers(env, input) : nullptr;
}

jobject JNICALL MC_getBuffer(JNIEnv* env, jobject thiz, jboolean input, jint index) {
  Codec* c = GetCodec(env, thiz);
  if (c == nullptr) return nullptr;
  if (!input) return c->OutputBuffer(env, index);
  jobject b = c->InputBuffer(env, index);
  return b;
}

jobject JNICALL MC_getImage(JNIEnv* env, jobject thiz, jboolean input, jint index) {
  Codec* c = GetCodec(env, thiz);
  if (c == nullptr || input) return nullptr;
  return c->OutputImage(env, index);
}

jstring JNICALL MC_getCanonicalName(JNIEnv* env, jobject thiz) {
  Codec* c = GetCodec(env, thiz);
  return c != nullptr ? env->NewStringUTF(c->entry().name.c_str()) : nullptr;
}

void JNICALL MC_setParameters(JNIEnv*, jobject, jobjectArray, jobjectArray) {}
void JNICALL MC_setVideoScalingMode(JNIEnv*, jobject, jint) {}

void JNICALL MC_enableOnFrameRenderedListener(JNIEnv* env, jobject thiz, jboolean enable) {
  if (Codec* c = GetCodec(env, thiz)) c->EnableFrameRendered(enable);
}

jobject JNICALL MC_getMetrics(JNIEnv* env, jobject) {
  jclass cls = env->FindClass("android/os/PersistableBundle");
  return env->NewObject(cls, env->GetMethodID(cls, "<init>", "()V"));
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
  // FLAG_IS_HARDWARE_ACCELERATED (8) or FLAG_IS_SOFTWARE_ONLY (4).
  return e->hardware ? 8 : 4;
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

// The profiles and levels a decoder declares, as on a device's codec list.
std::vector<std::pair<int, int>> ProfileLevels(const std::string& mime) {
  if (mime == "video/avc") return {{0x01, 0x8000}, {0x02, 0x8000}, {0x08, 0x8000}, {0x10000, 0x8000}, {0x80000, 0x8000}};
  if (mime == "video/hevc") return {{0x01, 0x10000}};
  if (mime == "video/x-vnd.on2.vp8") return {{0x01, 0x01}};
  if (mime == "video/x-vnd.on2.vp9") return {{0x01, 0x200}};
  if (mime == "video/av01") return {{0x1, 0x2000}};
  if (mime == "video/mp4v-es") return {{0x01, 0x80}};
  if (mime == "video/3gpp") return {{0x01, 0x80}};
  if (mime == "audio/mp4a-latm") return {{2, 0}, {5, 0}, {29, 0}};
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
  auto pls = ProfileLevels(e->mime);
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
    details.strings["block-size"] = "16x16";
    const int64_t blocks = static_cast<int64_t>((mw + 15) / 16) * ((mh + 15) / 16);
    details.strings["block-count-range"] = "1-" + std::to_string(blocks);
    details.strings["blocks-per-second-range"] = "1-" + std::to_string(blocks * 60);
    details.strings["bitrate-range"] = "1-120000000";
    details.strings["frame-rate-range"] = "1-120";
    details.ints["feature-adaptive-playback"] = 1;
  } else {
    details.strings["max-channel-count"] = "8";
    details.strings["sample-rate-ranges"] = "7350-96000";
    details.strings["bitrate-range"] = "8000-960000";
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
    impl = env->NewObject(impl_class, ctor, pl_array, color_array, JNI_FALSE, default_map, details_map);
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
    caps = env->NewObject(caps_class, ctor, pl_array, color_array, JNI_FALSE, default_map, details_map);
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
