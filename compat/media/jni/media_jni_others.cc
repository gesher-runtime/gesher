// Gesher: libmedia_jni's classes other than MediaPlayer (docs/media.md):
// MediaMetadataRetriever over the extractors (the container's metadata and
// the tracks'), MediaExtractor (media_extractor_jni.cc), and stand-ins for the codec, muxer, recorder and DRM classes
// until their stages (roadmap phase 8).

#include <jni.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "../audio/log.h"
#include "../player/data_source.h"
#include "../player/decoder.h"
#include "../player/extractor.h"
#include "../video/video_decoder.h"
#include "jni_util.h"
#include "java_data_source.h"
#include "receiver_lease.h"
#include "media_jni.h"

namespace gesher::media {
namespace {

// --- MediaMetadataRetriever ----------------------------------------------------------------

jfieldID gRetrieverContext;  // long mNativeContext
std::mutex gRetrieverContextLock;

struct RetrieverSnapshot {
  std::shared_ptr<DataSource> source;  // for frames (getFrameAtTime)
  std::map<int, std::string> metadata;
  std::vector<uint8_t> album_art;
};

struct Retriever : ReceiverState {
  RetrieverSnapshot snapshot;
  bool opening = false;
};

ReceiverLease<Retriever> GetRetriever(JNIEnv* env, jobject thiz) {
  return ReceiverLease<Retriever>::Acquire(env, thiz, gRetrieverContext, gRetrieverContextLock);
}

// MediaMetadataRetriever's keys, from the container's AMediaFormat keys.
const struct {
  int key;
  const char* name;
} kStringKeys[] = {
    {0, "cdtracknum"}, {1, "album"},    {2, "artist"},      {3, "author"},  {4, "composer"},
    {5, "date"},       {6, "genre"},    {7, "title"},       {8, "year"},    {11, "lyricist"},
    {13, "albumartist"}, {14, "discnum"}, {15, "compilation"}, {23, "location"},
};

bool Fill(RetrieverSnapshot* r, std::shared_ptr<DataSource> source, std::string* error) {
  int64_t size = source->Size();
  r->source = source;
  auto ex = Extractor::Open(source, error);
  if (ex == nullptr) return false;
  r->metadata.clear();
  Format file = ex->FileFormat();
  for (const auto& k : kStringKeys) {
    std::string v;
    if (file.GetString(k.name, &v) && !v.empty()) r->metadata[k.key] = v;
  }
  std::string mime = file.Mime();
  if (!mime.empty()) r->metadata[12] = mime;
  file.GetBuffer("albumart", &r->album_art);
  int64_t duration = -1;
  file.GetInt64("durationUs", &duration);
  bool audio = false, video = false;
  size_t tracks = ex->TrackCount();
  for (size_t i = 0; i < tracks; i++) {
    Format f = ex->TrackFormat(i);
    std::string m = f.Mime();
    int64_t d = -1;
    if (f.GetInt64("durationUs", &d) && d > duration) duration = d;
    if (m.rfind("audio/", 0) == 0 && !audio) {
      audio = true;
      int32_t v;
      if (f.GetInt32("sample-rate", &v)) r->metadata[38] = std::to_string(v);
      if (f.GetInt32("bits-per-sample", &v)) r->metadata[39] = std::to_string(v);
      if (mime.empty()) r->metadata[12] = m;
    } else if (m.rfind("video/", 0) == 0 && !video) {
      video = true;
      int32_t w, h, rot;
      if (f.GetInt32("width", &w)) r->metadata[18] = std::to_string(w);
      if (f.GetInt32("height", &h)) r->metadata[19] = std::to_string(h);
      r->metadata[24] = std::to_string(f.GetInt32("rotation-degrees", &rot) ? rot : 0);
      int32_t frame_count;
      if (f.GetInt32("frame-count", &frame_count) && frame_count > 0) {
        r->metadata[32] = std::to_string(frame_count);
      }
      r->metadata[40] = m;
    }
  }
  r->metadata[10] = std::to_string(tracks);
  if (audio) r->metadata[16] = "yes";
  if (video) r->metadata[17] = "yes";
  if (duration >= 0) {
    r->metadata[9] = std::to_string(duration / 1000);
    int32_t bitrate;
    if (file.GetInt32("bitrate", &bitrate)) {
      r->metadata[20] = std::to_string(bitrate);
    } else if (size > 0 && duration > 0) {
      // Stagefright uses a floating-point average. Avoid an overflowing
      // integer numerator while retaining large sources with a valid result.
      const double average = static_cast<double>(size) * 8E6 / static_cast<double>(duration);
      if (average < 0x1p63) r->metadata[20] = std::to_string(static_cast<int64_t>(average));
    }
  }
  return true;
}

void SetFromSource(JNIEnv* env, jobject thiz, std::unique_ptr<DataSource> source,
                   const std::string& error) {
  auto r = GetRetriever(env, thiz);
  if (r == nullptr) {
    jniThrow(env, "java/lang/IllegalStateException", "No retriever available");
    return;
  }
  if (r->opening) {
    jniThrow(env, "java/lang/IllegalStateException", "setDataSource is in progress");
    return;
  }
  std::string e = error;
  RetrieverSnapshot next;
  r->opening = true;
  const bool opened = source != nullptr && Fill(&next, std::move(source), &e);
  r->opening = false;
  if (!opened) {
    // MetadataRetrieverClient creates a fresh Stagefright retriever and only
    // publishes it on success. The public receiver retains its old snapshot
    // when opening a replacement fails, including its callback ownership.
    MEDIA_LOGE("MediaMetadataRetriever: %s", e.c_str());
    jniThrow(env, "java/lang/RuntimeException", "setDataSource failed");
    return;
  }
  // Retire the prior source only after all new metadata/artwork is published,
  // while the receiver lease still rejects nested operations from close().
  RetrieverSnapshot retired = std::move(r->snapshot);
  r->snapshot = std::move(next);
}

void JNICALL MMR_setDataSourcePath(JNIEnv* env, jobject thiz, jobject, jstring uri, jobjectArray keys,
                                   jobjectArray values) {
  if (uri == nullptr) {
    jniThrow(env, "java/lang/IllegalArgumentException", "Null pointer");
    return;
  }
  std::string p = Utf8(env, uri);
  std::map<std::string, std::string> headers;
  if (keys != nullptr && values != nullptr) {
    jsize n = std::min(env->GetArrayLength(keys), env->GetArrayLength(values));
    for (jsize i = 0; i < n; i++) {
      headers[Utf8(env, static_cast<jstring>(env->GetObjectArrayElement(keys, i)))] =
          Utf8(env, static_cast<jstring>(env->GetObjectArrayElement(values, i)));
    }
  }
  std::string error;
  std::unique_ptr<DataSource> source;
  if (p.rfind("http://", 0) == 0 || p.rfind("https://", 0) == 0) {
    source = OpenHttpSource(p, headers, &error);
  } else {
    source = OpenFileSource(p, &error);
  }
  SetFromSource(env, thiz, std::move(source), error);
}

void JNICALL MMR_setDataSourceFD(JNIEnv* env, jobject thiz, jobject fileDescriptor, jlong offset,
                                 jlong length) {
  if (GetRetriever(env, thiz) == nullptr) {
    jniThrow(env, "java/lang/IllegalStateException", "No retriever available");
    return;
  }
  if (fileDescriptor == nullptr || offset < 0 || length < 0) {
    jniThrow(env, "java/lang/IllegalArgumentException", nullptr);
    return;
  }
  jclass fdc = env->FindClass("java/io/FileDescriptor");
  jint fd = env->GetIntField(fileDescriptor, env->GetFieldID(fdc, "descriptor", "I"));
  if (fd < 0) {
    jniThrow(env, "java/lang/IllegalArgumentException", nullptr);
    return;
  }
  std::string error;
  auto source = OpenFdSource(fd, offset, length >= 0x7fffffffffffffLL ? -1 : length, &error);
  SetFromSource(env, thiz, std::move(source), error);
}

void JNICALL MMR_setDataSourceCallback(JNIEnv* env, jobject thiz, jobject data_source) {
  if (GetRetriever(env, thiz) == nullptr) {
    jniThrow(env, "java/lang/IllegalStateException", "No retriever available");
    return;
  }
  auto source = OpenJavaDataSource(env, data_source);
  if (source == nullptr) return;
  SetFromSource(env, thiz, std::move(source), "MediaDataSource");
}

jstring JNICALL MMR_extractMetadata(JNIEnv* env, jobject thiz, jint key) {
  auto r = GetRetriever(env, thiz);
  if (r == nullptr) {
    jniThrow(env, "java/lang/IllegalStateException", "No retriever available");
    return nullptr;
  }
  auto it = r->snapshot.metadata.find(key);
  return it != r->snapshot.metadata.end() ? env->NewStringUTF(it->second.c_str()) : nullptr;
}

jbyteArray JNICALL MMR_getEmbeddedPicture(JNIEnv* env, jobject thiz, jint) {
  auto r = GetRetriever(env, thiz);
  if (r == nullptr) {
    jniThrow(env, "java/lang/IllegalStateException", "No retriever available");
    return nullptr;
  }
  if (r->snapshot.album_art.empty() ||
      r->snapshot.album_art.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) return nullptr;
  jbyteArray a = env->NewByteArray(static_cast<jsize>(r->snapshot.album_art.size()));
  if (a == nullptr) return nullptr;
  env->SetByteArrayRegion(a, 0, static_cast<jsize>(r->snapshot.album_art.size()),
                          reinterpret_cast<const jbyte*>(r->snapshot.album_art.data()));
  return a;
}

// FrameDecoder uses the extractor's target time for closest and indexed seeks.
// Decoders can emit multiple presentation-ordered frames at once, including at EOS.
template <typename Consumer>
void VisitRetrieverFrames(TrackReader* reader, VideoDecoder* decoder, int64_t position,
                          SeekMode mode, int input_limit, Consumer consume) {
  Sample sample;
  if (reader->Read(&sample, position, mode) != 0) return;
  if (mode == SeekMode::kFrameIndex && sample.target_time_us < 0) return;
  int64_t target_time = -1;
  if (mode == SeekMode::kClosest || mode == SeekMode::kFrameIndex) {
    target_time = sample.target_time_us >= 0 ? sample.target_time_us : position;
  }

  auto visit = [&](std::vector<VideoFrame>& frames) {
    for (VideoFrame& frame : frames) {
      if ((target_time < 0 || frame.time_us >= target_time) && !consume(frame)) return false;
    }
    return true;
  };
  std::vector<VideoFrame> frames;
  for (;;) {
    if (!decoder->Decode(sample, &frames) || !visit(frames)) return;
    frames.clear();
    int status = reader->Read(&sample);
    if (status < 0) return;
    if (status > 0) {
      decoder->Drain(&frames);
      visit(frames);
      return;
    }
    if (input_limit > 0 && --input_limit == 0) return;
  }
}

std::unique_ptr<VideoFrame> DecodeRetrieverFrame(TrackReader* reader, VideoDecoder* decoder,
                                               int64_t time_us, int option) {
  auto mode = static_cast<SeekMode>(std::clamp(option, 0, 3));
  std::unique_ptr<VideoFrame> chosen;
  VisitRetrieverFrames(reader, decoder, std::max<int64_t>(time_us, 0), mode, 600,
                       [&](VideoFrame& frame) {
                         chosen = std::make_unique<VideoFrame>(std::move(frame));
                         return false;
                       });
  return chosen;
}

jobject RetrieverBitmapConfig(JNIEnv* env, jobject params, jfieldID* actual_field) {
  constexpr const char* kConfigType = "Landroid/graphics/Bitmap$Config;";
  jclass config = env->FindClass("android/graphics/Bitmap$Config");
  if (config == nullptr) return nullptr;
  jfieldID argb_field = env->GetStaticFieldID(config, "ARGB_8888", kConfigType);
  if (argb_field == nullptr) return nullptr;
  if (params != nullptr) {
    jclass params_class = env->GetObjectClass(params);
    jfieldID preferred_field = env->GetFieldID(params_class, "inPreferredConfig", kConfigType);
    if (preferred_field == nullptr) return nullptr;
    *actual_field = env->GetFieldID(params_class, "outActualConfig", kConfigType);
    if (*actual_field == nullptr) return nullptr;
    jfieldID rgb565_field = env->GetStaticFieldID(config, "RGB_565", kConfigType);
    if (rgb565_field == nullptr) return nullptr;
    jobject preferred = env->GetObjectField(params, preferred_field);
    jobject rgb565 = env->GetStaticObjectField(config, rgb565_field);
    if (env->IsSameObject(preferred, rgb565)) return rgb565;
  }
  return env->GetStaticObjectField(config, argb_field);
}

// Clockwise rotation from coded pixels to presentation orientation.
size_t RetrieverPixelIndex(int x, int y, int w, int h, int rotation) {
  switch (rotation) {
    case 90: return static_cast<size_t>(x) * h + (h - 1 - y);
    case 180: return static_cast<size_t>(h - 1 - y) * w + (w - 1 - x);
    case 270: return static_cast<size_t>(w - 1 - x) * h + y;
    default: return static_cast<size_t>(y) * w + x;
  }
}

// FrameDecoder::allocVideoFrame uses track presentation metadata after the
// decoder's crop. A valid track crop overrides SAR and explicit display size.
bool RetrieverDisplaySize(const VideoFrame& frame, const Format& track, int32_t* width,
                          int32_t* height) {
  const int w = frame.visible_width(), h = frame.visible_height();
  if (w <= 0 || h <= 0) return false;
  int32_t sar_w, sar_h;
  if (track.GetInt32("sar-width", &sar_w) && track.GetInt32("sar-height", &sar_h) && sar_h != 0) {
    int32_t product;
    if (sar_w < 0 || __builtin_mul_overflow(w, sar_w, &product)) return false;
    *width = product / sar_h;
    *height = h;
  } else if (!track.GetInt32("display-width", width) || !track.GetInt32("display-height", height)
             || *width <= 0 || *height <= 0) {
    *width = w;
    *height = h;
  }
  int32_t left, top, right, bottom;
  if (track.GetRect("crop", &left, &top, &right, &bottom) && left >= 0 && top >= 0
      && right < w && bottom < h && left <= right && top <= bottom) {
    *width = right - left + 1;
    *height = bottom - top + 1;
  }
  return true;
}

jobject CreateRetrieverBitmap(JNIEnv* env, const VideoFrame& frame, jobject config,
                              int dst_w, int dst_h, int rotation, int display_w, int display_h) {
  const int w = frame.visible_width(), h = frame.visible_height();
  int bitmap_w = w, bitmap_h = h;
  if (rotation == 90 || rotation == 270) {
    std::swap(bitmap_w, bitmap_h);
    std::swap(display_w, display_h);
  }
  const size_t pixel_count = static_cast<size_t>(w) * h;
  if (pixel_count > static_cast<size_t>(std::numeric_limits<jsize>::max()) / sizeof(jint)) {
    return nullptr;
  }
  if (env->PushLocalFrame(4) < 0) {
    env->ExceptionClear();
    return nullptr;
  }
  const jsize length = static_cast<jsize>(pixel_count);
  jintArray colors = env->NewIntArray(length);
  if (colors == nullptr) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    return env->PopLocalFrame(nullptr);
  }
  std::unique_ptr<uint32_t[]> rgba(new (std::nothrow) uint32_t[pixel_count]);
  std::unique_ptr<jint[]> argb(new (std::nothrow) jint[pixel_count]);
  // Scratch storage belongs to the initial bitmap allocation. Like the Android
  // bitmap allocation failure below, it produces a null frame without an exception.
  if (rgba == nullptr || argb == nullptr) return env->PopLocalFrame(nullptr);
  I420ToRgba(frame, rgba.get(), w, w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint32_t p = rgba[static_cast<size_t>(y) * w + x];
      argb[RetrieverPixelIndex(x, y, w, h, rotation)] = static_cast<jint>(
          0xff000000u | ((p & 0xffu) << 16) | (p & 0xff00u) | ((p >> 16) & 0xffu));
    }
  }
  env->SetIntArrayRegion(colors, 0, length, argb.get());
  if (env->ExceptionCheck()) return env->PopLocalFrame(nullptr);
  jclass bitmap = env->FindClass("android/graphics/Bitmap");
  if (bitmap == nullptr) return env->PopLocalFrame(nullptr);
  jmethodID create = env->GetStaticMethodID(
      bitmap, "createBitmap", "([IIILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
  if (create == nullptr) return env->PopLocalFrame(nullptr);
  jobject result = env->CallStaticObjectMethod(bitmap, create, colors, bitmap_w, bitmap_h, config);
  if (result == nullptr) {
    // AOSP clears failure to allocate the decoded frame's initial Java bitmap.
    if (env->ExceptionCheck()) env->ExceptionClear();
    return env->PopLocalFrame(nullptr);
  }
  if (dst_w <= 0 || dst_h <= 0) {
    dst_w = display_w;
    dst_h = display_h;
  } else {
    const float factor = std::min(static_cast<float>(dst_w) / display_w,
                                  static_cast<float>(dst_h) / display_h);
    dst_w = std::round(display_w * factor);
    dst_h = std::round(display_h * factor);
  }
  if (dst_w != bitmap_w || dst_h != bitmap_h) {
    jmethodID scale = env->GetStaticMethodID(
        bitmap, "createScaledBitmap", "(Landroid/graphics/Bitmap;IIZ)Landroid/graphics/Bitmap;");
    if (scale == nullptr) return env->PopLocalFrame(nullptr);
    result = env->CallStaticObjectMethod(bitmap, scale, result, dst_w, dst_h, true);
  }
  return env->PopLocalFrame(result);
}

jobject JNICALL MMR_getFrameAtTime(JNIEnv* env, jobject thiz, jlong time_us, jint option, jint dst_w,
                                   jint dst_h, jobject params) {
  auto r = GetRetriever(env, thiz);
  if (r == nullptr || r->snapshot.source == nullptr) {
    jniThrow(env, "java/lang/IllegalStateException", "No retriever available");
    return nullptr;
  }
  std::string error;
  auto ex = Extractor::Open(r->snapshot.source, &error);
  if (ex == nullptr) return nullptr;
  std::unique_ptr<VideoDecoder> decoder;
  std::unique_ptr<TrackReader> reader;
  Format track;
  int32_t rotation = 0;
  for (size_t i = 0; i < ex->TrackCount() && decoder == nullptr; i++) {
    Format f = ex->TrackFormat(i);
    if (f.Mime().rfind("video/", 0) != 0) continue;
    decoder = VideoDecoder::Create(f, &error);
    if (decoder != nullptr) {
      reader = ex->OpenTrack(i);
      f.GetInt32("rotation-degrees", &rotation);
      track = std::move(f);
    }
  }
  if (decoder == nullptr || reader == nullptr) {
    MEDIA_LOGW("MediaMetadataRetriever: no video frame: %s", error.c_str());
    return nullptr;
  }
  auto chosen = DecodeRetrieverFrame(reader.get(), decoder.get(), time_us, option);
  if (chosen == nullptr) return nullptr;
  int32_t display_w, display_h;
  if (!RetrieverDisplaySize(*chosen, track, &display_w, &display_h)) return nullptr;

  jfieldID actual_field = nullptr;
  jobject config = RetrieverBitmapConfig(env, params, &actual_field);
  if (config == nullptr) return nullptr;
  if (actual_field != nullptr) env->SetObjectField(params, actual_field, config);
  return CreateRetrieverBitmap(env, *chosen, config, dst_w, dst_h, rotation, display_w, display_h);
}

jobject JNICALL MMR_getFrameAtIndex(JNIEnv* env, jobject thiz, jint frame_index, jint frame_count,
                                    jobject params) {
  auto r = GetRetriever(env, thiz);
  if (r == nullptr || r->snapshot.source == nullptr) {
    jniThrow(env, "java/lang/IllegalStateException", "No retriever available");
    return nullptr;
  }

  jclass list_class = env->FindClass("java/util/ArrayList");
  if (list_class == nullptr) return nullptr;
  jmethodID init = env->GetMethodID(list_class, "<init>", "()V");
  if (init == nullptr) return nullptr;
  jmethodID add = env->GetMethodID(list_class, "add", "(Ljava/lang/Object;)Z");
  if (add == nullptr) return nullptr;
  jobject list = env->NewObject(list_class, init);
  if (list == nullptr) {
    // Android's jniThrowException discards the original allocation exception.
    env->ExceptionClear();
    jniThrow(env, "java/lang/IllegalStateException", "Can't create bitmap array");
    return nullptr;
  }
  jfieldID actual_field = nullptr;
  jobject config = RetrieverBitmapConfig(env, params, &actual_field);
  if (config == nullptr) return nullptr;
  if (actual_field != nullptr) env->SetObjectField(params, actual_field, config);

  std::string error;
  auto ex = Extractor::Open(r->snapshot.source, &error);
  jint count = 0;
  if (ex != nullptr) {
    for (size_t i = 0; i < ex->TrackCount(); ++i) {
      Format format = ex->TrackFormat(i);
      if (format.Mime().rfind("video/", 0) != 0) continue;
      int32_t available, rotation = 0;
      if (!format.GetInt32("frame-count", &available) || available <= 0) break;
      if (frame_index < 0 || frame_index >= available || frame_count < 1 ||
          frame_count > available - frame_index) {
        jniThrow(env, "java/lang/IllegalArgumentException", "Invalid frameIndex or numFrames");
        return nullptr;
      }
      format.GetInt32("rotation-degrees", &rotation);
      auto decoder = VideoDecoder::Create(format, &error);
      auto reader = ex->OpenTrack(i);
      if (decoder == nullptr || reader == nullptr) break;
      VisitRetrieverFrames(reader.get(), decoder.get(), frame_index, SeekMode::kFrameIndex, 0,
                           [&](const VideoFrame& frame) {
                             int32_t display_w, display_h;
                             if (!RetrieverDisplaySize(frame, format, &display_w, &display_h)) return false;
                             jobject bitmap =
                                 CreateRetrieverBitmap(env, frame, config, -1, -1, rotation,
                                                       display_w, display_h);
                             if (env->ExceptionCheck()) return false;
                             env->CallBooleanMethod(list, add, bitmap);
                             env->DeleteLocalRef(bitmap);
                             if (env->ExceptionCheck()) return false;
                             return ++count < frame_count;
                           });
      break;
    }
  }
  if (env->ExceptionCheck()) return nullptr;
  if (count == 0) {
    jniThrow(env, "java/lang/IllegalStateException", "No frames from retriever");
    return nullptr;
  }
  return list;
}

void JNICALL MMR_init(JNIEnv* env, jclass) {
  jclass cls = env->FindClass("android/media/MediaMetadataRetriever");
  gRetrieverContext = env->GetFieldID(cls, "mNativeContext", "J");
}

void JNICALL MMR_setup(JNIEnv* env, jobject thiz) {
  ReceiverLease<Retriever>::Setup(env, thiz, gRetrieverContext, gRetrieverContextLock);
}

void JNICALL MMR_release(JNIEnv* env, jobject thiz) {
  ReceiverLease<Retriever>::Release(env, thiz, gRetrieverContext, gRetrieverContextLock);
}

const JNINativeMethod kRetrieverMethods[] = {
    {"_setDataSource",
     "(Landroid/os/IBinder;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V",
     GESHER_NATIVE(MMR_setDataSourcePath)},
    {"_setDataSource", "(Ljava/io/FileDescriptor;JJ)V", GESHER_NATIVE(MMR_setDataSourceFD)},
    {"_setDataSource", "(Landroid/media/MediaDataSource;)V", GESHER_NATIVE(MMR_setDataSourceCallback)},
    {"nativeExtractMetadata", "(I)Ljava/lang/String;", GESHER_NATIVE(MMR_extractMetadata)},
    {"getEmbeddedPicture", "(I)[B", GESHER_NATIVE(MMR_getEmbeddedPicture)},
    {"_getFrameAtTime", "(JIIILandroid/media/MediaMetadataRetriever$BitmapParams;)Landroid/graphics/Bitmap;",
     GESHER_NATIVE(MMR_getFrameAtTime)},
    {"_getFrameAtIndex", "(IILandroid/media/MediaMetadataRetriever$BitmapParams;)Ljava/util/List;",
     GESHER_NATIVE(MMR_getFrameAtIndex)},
    {"release", "()V", GESHER_NATIVE(MMR_release)},
    {"native_finalize", "()V", GESHER_NATIVE(MMR_release)},
    {"native_setup", "()V", GESHER_NATIVE(MMR_setup)},
    {"native_init", "()V", GESHER_NATIVE(MMR_init)},
};

}  // namespace

int RegisterMediaJniOthers(JNIEnv* env) {
  if (RegisterStandins(env, "android/media/MediaMetadataRetriever")) {
    RegisterMethods(env, "android/media/MediaMetadataRetriever", kRetrieverMethods);
  }
  // Until their stages (phase 8, stages 2 and 3): every native answers zero,
  // false or null.
  RegisterMediaExtractor(env);
  RegisterMediaCodec(env);
  // Recording (media3, ADR 0059): MediaMuxer, MediaRecorder.
  RegisterMediaMuxer(env);
  RegisterMediaRecorder(env);
  for (const char* cls : {"android/media/MediaSync",
                          "android/media/MediaHTTPConnection", "android/media/MediaCrypto",
                          "android/media/MediaDrm", "android/media/MediaDescrambler",
                          "android/media/MediaProfiles", "android/media/JetPlayer",
                          "android/media/ResampleInputStream", "android/media/MediaCodec$LinearBlock",
                          "android/media/MediaCodecInfo$CodecCapabilities"}) {
    RegisterStandins(env, cls);
  }
  return 0;
}

}  // namespace gesher::media
