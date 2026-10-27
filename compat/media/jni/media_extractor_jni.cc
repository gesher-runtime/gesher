// Gesher: android.media.MediaExtractor's natives over the extractor wrapper
// (compat/media/player/extractor.h; docs/media.md), in libmedia_jni.so.
//
// As NuMediaExtractor: the selected tracks are read ahead one sample each,
// and the current sample is the earliest of them; formats go to Java as the
// Map<String, Object> MediaFormat is built from (Integer, Long, Float, String,
// ByteBuffer values, the NDK's keys).

#include <jni.h>

#include <media/NdkMediaFormat.h>
#include <media/NdkMediaFormatPriv.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../audio/log.h"
#include "../player/data_source.h"
#include "../player/extractor.h"
#include "../video/video_decoder.h"
#include "jni_util.h"
#include "media_jni.h"

namespace gesher::media {
namespace {

jfieldID gContext;  // long mNativeContext

struct JniExtractor {
  std::shared_ptr<DataSource> source;
  std::unique_ptr<Extractor> extractor;
  struct Selected {
    std::unique_ptr<TrackReader> reader;
    Sample sample;
    bool have = false;
    bool eos = false;
  };
  std::map<size_t, Selected> selected;

  void Fill(Selected& s) {
    if (s.have || s.eos) return;
    int r = s.reader->Read(&s.sample);
    s.have = r == 0;
    s.eos = r != 0;
  }

  // The selected track with the earliest pending sample, or -1.
  long Current() {
    long best = -1;
    int64_t t = 0;
    for (auto& [index, s] : selected) {
      Fill(s);
      if (s.have && (best < 0 || s.sample.time_us < t)) {
        best = static_cast<long>(index);
        t = s.sample.time_us;
      }
    }
    return best;
  }
};

JniExtractor* Get(JNIEnv* env, jobject thiz) {
  auto* e = reinterpret_cast<JniExtractor*>(env->GetLongField(thiz, gContext));
  if (e == nullptr) jniThrow(env, "java/lang/IllegalStateException", nullptr);
  return e;
}

JniExtractor* GetOpened(JNIEnv* env, jobject thiz) {
  JniExtractor* e = Get(env, thiz);
  if (e != nullptr && e->extractor == nullptr) {
    jniThrow(env, "java/lang/IllegalStateException", "no data source");
    return nullptr;
  }
  return e;
}

jobject Boxed(JNIEnv* env, const char* cls, const char* sig, jvalue v) {
  jclass c = env->FindClass(cls);
  jmethodID valueOf = env->GetStaticMethodID(c, "valueOf", sig);
  return env->CallStaticObjectMethodA(c, valueOf, &v);
}

}  // namespace

// An AMediaFormat as the HashMap MediaFormat(Map) takes.
jobject FormatToMap(JNIEnv* env, const Format& f) {
  jclass hashMap = env->FindClass("java/util/HashMap");
  jobject map = env->NewObject(hashMap, env->GetMethodID(hashMap, "<init>", "()V"));
  jmethodID put = env->GetMethodID(hashMap, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
  if (f.get() == nullptr) return map;
  const android::sp<android::AMessage>& msg = f.get()->mFormat;
  for (size_t i = 0; i < msg->countEntries(); i++) {
    android::AMessage::Type type;
    const char* name = msg->getEntryNameAt(i, &type);
    jobject value = nullptr;
    jvalue v;
    switch (type) {
      case android::AMessage::kTypeInt32: {
        int32_t x;
        msg->findInt32(name, &x);
        v.i = x;
        value = Boxed(env, "java/lang/Integer", "(I)Ljava/lang/Integer;", v);
        break;
      }
      case android::AMessage::kTypeInt64: {
        int64_t x;
        msg->findInt64(name, &x);
        v.j = x;
        value = Boxed(env, "java/lang/Long", "(J)Ljava/lang/Long;", v);
        break;
      }
      case android::AMessage::kTypeFloat: {
        float x;
        msg->findFloat(name, &x);
        v.f = x;
        value = Boxed(env, "java/lang/Float", "(F)Ljava/lang/Float;", v);
        break;
      }
      case android::AMessage::kTypeString: {
        android::AString x;
        msg->findString(name, &x);
        value = env->NewStringUTF(x.c_str());
        break;
      }
      case android::AMessage::kTypeBuffer: {
        android::sp<android::ABuffer> b;
        msg->findBuffer(name, &b);
        if (b == nullptr) break;
        jbyteArray a = env->NewByteArray(static_cast<jsize>(b->size()));
        env->SetByteArrayRegion(a, 0, static_cast<jsize>(b->size()),
                                reinterpret_cast<const jbyte*>(b->data()));
        jclass bb = env->FindClass("java/nio/ByteBuffer");
        value = env->CallStaticObjectMethod(bb, env->GetStaticMethodID(bb, "wrap", "([B)Ljava/nio/ByteBuffer;"), a);
        break;
      }
      default:
        break;
    }
    if (value == nullptr) continue;
    jstring key = env->NewStringUTF(name);
    env->DeleteLocalRef(env->CallObjectMethod(map, put, key, value));
    env->DeleteLocalRef(key);
    env->DeleteLocalRef(value);
  }
  return map;
}

namespace {

jobject ToMap(JNIEnv* env, const Format& f) { return FormatToMap(env, f); }

// A track's format as NuMediaExtractor gives it: H.264's avcC as csd-0 (the
// SPSs) and csd-1 (the PPSs), H.265's hvcC as csd-0, with start codes.
Format TrackFormatForJava(Extractor* ex, size_t index) {
  Format f = ex->TrackFormat(index);
  std::string mime = f.Mime();
  std::vector<uint8_t> b, have;
  if (mime == "video/avc" && f.GetBuffer("csd-avc", &b) && !f.GetBuffer("csd-0", &have)) {
    std::vector<uint8_t> sps, pps;
    if (AvccToAnnexB(b.data(), b.size(), &sps, &pps)) {
      AMediaFormat_setBuffer(f.get(), "csd-0", sps.data(), sps.size());
      if (!pps.empty()) AMediaFormat_setBuffer(f.get(), "csd-1", pps.data(), pps.size());
    }
  } else if (mime == "video/hevc" && f.GetBuffer("csd-hevc", &b) && !f.GetBuffer("csd-0", &have)) {
    std::vector<uint8_t> ps;
    if (HvccToAnnexB(b.data(), b.size(), &ps)) AMediaFormat_setBuffer(f.get(), "csd-0", ps.data(), ps.size());
  } else if (mime == "video/mp4v-es") {
    AddCsdFromEsds(f);  // MPEG-4 part 2's VOL header
  }
  return f;
}

void Open(JNIEnv* env, jobject thiz, std::unique_ptr<DataSource> source, const std::string& error) {
  JniExtractor* e = Get(env, thiz);
  if (e == nullptr) return;
  std::string err = error;
  if (source != nullptr) {
    e->source = std::move(source);
    e->extractor = Extractor::Open(e->source, &err);
  }
  if (e->extractor == nullptr) {
    MEDIA_LOGE("MediaExtractor: %s", err.c_str());
    jniThrow(env, "java/io/IOException", "Failed to instantiate extractor.");
  }
}

void JNICALL ME_setDataSourcePath(JNIEnv* env, jobject thiz, jobject, jstring path, jobjectArray keys,
                                  jobjectArray values) {
  std::string p = Utf8(env, path);
  std::map<std::string, std::string> headers;
  if (keys != nullptr && values != nullptr) {
    jsize n = std::min(env->GetArrayLength(keys), env->GetArrayLength(values));
    for (jsize i = 0; i < n; i++) {
      headers[Utf8(env, static_cast<jstring>(env->GetObjectArrayElement(keys, i)))] =
          Utf8(env, static_cast<jstring>(env->GetObjectArrayElement(values, i)));
    }
  }
  std::string error;
  std::unique_ptr<DataSource> s = p.rfind("http", 0) == 0 ? OpenHttpSource(p, headers, &error)
                                                          : OpenFileSource(p, &error);
  Open(env, thiz, std::move(s), error);
}

void JNICALL ME_setDataSourceFD(JNIEnv* env, jobject thiz, jobject fileDescriptor, jlong offset,
                                jlong length) {
  if (fileDescriptor == nullptr) {
    jniThrow(env, "java/lang/IllegalArgumentException", nullptr);
    return;
  }
  jclass fdc = env->FindClass("java/io/FileDescriptor");
  jint fd = env->GetIntField(fileDescriptor, env->GetFieldID(fdc, "descriptor", "I"));
  std::string error;
  Open(env, thiz, OpenFdSource(fd, offset, length >= 0x7fffffffffffffLL ? -1 : length, &error), error);
}

jint JNICALL ME_getTrackCount(JNIEnv* env, jobject thiz) {
  JniExtractor* e = GetOpened(env, thiz);
  return e != nullptr ? static_cast<jint>(e->extractor->TrackCount()) : 0;
}

jobject JNICALL ME_getTrackFormat(JNIEnv* env, jobject thiz, jint index) {
  JniExtractor* e = GetOpened(env, thiz);
  if (e == nullptr) return nullptr;
  if (index < 0 || static_cast<size_t>(index) >= e->extractor->TrackCount()) {
    jniThrow(env, "java/lang/IllegalArgumentException", nullptr);
    return nullptr;
  }
  return ToMap(env, TrackFormatForJava(e->extractor.get(), static_cast<size_t>(index)));
}

jobject JNICALL ME_getFileFormat(JNIEnv* env, jobject thiz) {
  JniExtractor* e = GetOpened(env, thiz);
  return e != nullptr ? ToMap(env, e->extractor->FileFormat()) : nullptr;
}

void JNICALL ME_selectTrack(JNIEnv* env, jobject thiz, jint index) {
  JniExtractor* e = GetOpened(env, thiz);
  if (e == nullptr) return;
  if (index < 0 || static_cast<size_t>(index) >= e->extractor->TrackCount()) {
    jniThrow(env, "java/lang/IllegalArgumentException", nullptr);
    return;
  }
  if (e->selected.count(static_cast<size_t>(index)) != 0) return;
  JniExtractor::Selected s;
  s.reader = e->extractor->OpenTrack(static_cast<size_t>(index));
  if (s.reader == nullptr) {
    jniThrow(env, "java/lang/IllegalArgumentException", "track not readable");
    return;
  }
  e->selected[static_cast<size_t>(index)] = std::move(s);
}

void JNICALL ME_unselectTrack(JNIEnv* env, jobject thiz, jint index) {
  if (JniExtractor* e = GetOpened(env, thiz)) e->selected.erase(static_cast<size_t>(index));
}

void JNICALL ME_seekTo(JNIEnv* env, jobject thiz, jlong time_us, jint mode) {
  JniExtractor* e = GetOpened(env, thiz);
  if (e == nullptr) return;
  for (auto& [index, s] : e->selected) {
    int r = s.reader->Read(&s.sample, std::max<jlong>(time_us, 0), static_cast<SeekMode>(mode));
    s.have = r == 0;
    s.eos = r != 0;
  }
}

jboolean JNICALL ME_advance(JNIEnv* env, jobject thiz) {
  JniExtractor* e = GetOpened(env, thiz);
  if (e == nullptr) return JNI_FALSE;
  long cur = e->Current();
  if (cur < 0) return JNI_FALSE;
  e->selected[static_cast<size_t>(cur)].have = false;
  return e->Current() >= 0 ? JNI_TRUE : JNI_FALSE;
}

jint JNICALL ME_readSampleData(JNIEnv* env, jobject thiz, jobject buffer, jint offset) {
  JniExtractor* e = GetOpened(env, thiz);
  if (e == nullptr) return -1;
  long cur = e->Current();
  if (cur < 0) return -1;
  const Sample& s = e->selected[static_cast<size_t>(cur)].sample;
  jclass bbc = env->FindClass("java/nio/ByteBuffer");
  jlong capacity = env->GetDirectBufferCapacity(buffer);
  uint8_t* dst = static_cast<uint8_t*>(env->GetDirectBufferAddress(buffer));
  if (dst == nullptr) {
    // A heap buffer: through its array.
    jbyteArray array = static_cast<jbyteArray>(
        env->CallObjectMethod(buffer, env->GetMethodID(bbc, "array", "()[B")));
    jint array_offset = env->CallIntMethod(buffer, env->GetMethodID(bbc, "arrayOffset", "()I"));
    if (array == nullptr || env->ExceptionCheck()) return -1;
    capacity = env->GetArrayLength(array) - array_offset;
    if (offset < 0 || offset + static_cast<jlong>(s.data.size()) > capacity) {
      jniThrow(env, "java/lang/IllegalArgumentException", "sample larger than the buffer");
      return -1;
    }
    env->SetByteArrayRegion(array, array_offset + offset, static_cast<jsize>(s.data.size()),
                            reinterpret_cast<const jbyte*>(s.data.data()));
  } else {
    if (offset < 0 || offset + static_cast<jlong>(s.data.size()) > capacity) {
      jniThrow(env, "java/lang/IllegalArgumentException", "sample larger than the buffer");
      return -1;
    }
    memcpy(dst + offset, s.data.data(), s.data.size());
  }
  // position = offset, limit = offset + size, as NuMediaExtractor's caller sets them.
  jmethodID limit = env->GetMethodID(bbc, "limit", "(I)Ljava/nio/Buffer;");
  jmethodID position = env->GetMethodID(bbc, "position", "(I)Ljava/nio/Buffer;");
  env->DeleteLocalRef(env->CallObjectMethod(buffer, limit, offset + static_cast<jint>(s.data.size())));
  env->DeleteLocalRef(env->CallObjectMethod(buffer, position, offset));
  return static_cast<jint>(s.data.size());
}

jint JNICALL ME_getSampleTrackIndex(JNIEnv* env, jobject thiz) {
  JniExtractor* e = GetOpened(env, thiz);
  return e != nullptr ? static_cast<jint>(e->Current()) : -1;
}

jlong JNICALL ME_getSampleTime(JNIEnv* env, jobject thiz) {
  JniExtractor* e = GetOpened(env, thiz);
  if (e == nullptr) return -1;
  long cur = e->Current();
  return cur >= 0 ? e->selected[static_cast<size_t>(cur)].sample.time_us : -1;
}

jlong JNICALL ME_getSampleSize(JNIEnv* env, jobject thiz) {
  JniExtractor* e = GetOpened(env, thiz);
  if (e == nullptr) return -1;
  long cur = e->Current();
  return cur >= 0 ? static_cast<jlong>(e->selected[static_cast<size_t>(cur)].sample.data.size()) : -1;
}

jint JNICALL ME_getSampleFlags(JNIEnv* env, jobject thiz) {
  JniExtractor* e = GetOpened(env, thiz);
  if (e == nullptr) return -1;
  long cur = e->Current();
  if (cur < 0) return -1;
  return e->selected[static_cast<size_t>(cur)].sample.sync ? 1 /* SAMPLE_FLAG_SYNC */ : 0;
}

jlong JNICALL ME_getCachedDuration(JNIEnv*, jobject) { return -1; }
jboolean JNICALL ME_hasCacheReachedEndOfStream(JNIEnv*, jobject) { return JNI_TRUE; }

void JNICALL ME_init(JNIEnv* env, jclass) {
  gContext = env->GetFieldID(env->FindClass("android/media/MediaExtractor"), "mNativeContext", "J");
}
void JNICALL ME_setup(JNIEnv* env, jobject thiz) {
  env->SetLongField(thiz, gContext, reinterpret_cast<jlong>(new JniExtractor));
}
void JNICALL ME_release(JNIEnv* env, jobject thiz) {
  delete reinterpret_cast<JniExtractor*>(env->GetLongField(thiz, gContext));
  env->SetLongField(thiz, gContext, 0);
}

jobject JNICALL ME_getAudioPresentations(JNIEnv* env, jobject, jint) {
  jclass c = env->FindClass("java/util/ArrayList");
  return env->NewObject(c, env->GetMethodID(c, "<init>", "()V"));
}

const JNINativeMethod kMethods[] = {
    {"nativeSetDataSource",
     "(Landroid/os/IBinder;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V",
     GESHER_NATIVE(ME_setDataSourcePath)},
    {"setDataSource", "(Ljava/io/FileDescriptor;JJ)V", GESHER_NATIVE(ME_setDataSourceFD)},
    {"getTrackCount", "()I", GESHER_NATIVE(ME_getTrackCount)},
    {"getTrackFormatNative", "(I)Ljava/util/Map;", GESHER_NATIVE(ME_getTrackFormat)},
    {"getFileFormatNative", "()Ljava/util/Map;", GESHER_NATIVE(ME_getFileFormat)},
    {"selectTrack", "(I)V", GESHER_NATIVE(ME_selectTrack)},
    {"unselectTrack", "(I)V", GESHER_NATIVE(ME_unselectTrack)},
    {"seekTo", "(JI)V", GESHER_NATIVE(ME_seekTo)},
    {"advance", "()Z", GESHER_NATIVE(ME_advance)},
    {"readSampleData", "(Ljava/nio/ByteBuffer;I)I", GESHER_NATIVE(ME_readSampleData)},
    {"getSampleTrackIndex", "()I", GESHER_NATIVE(ME_getSampleTrackIndex)},
    {"getSampleTime", "()J", GESHER_NATIVE(ME_getSampleTime)},
    {"getSampleSize", "()J", GESHER_NATIVE(ME_getSampleSize)},
    {"getSampleFlags", "()I", GESHER_NATIVE(ME_getSampleFlags)},
    {"getCachedDuration", "()J", GESHER_NATIVE(ME_getCachedDuration)},
    {"hasCacheReachedEndOfStream", "()Z", GESHER_NATIVE(ME_hasCacheReachedEndOfStream)},
    {"native_init", "()V", GESHER_NATIVE(ME_init)},
    {"native_setup", "()V", GESHER_NATIVE(ME_setup)},
    {"native_finalize", "()V", GESHER_NATIVE(ME_release)},
    {"release", "()V", GESHER_NATIVE(ME_release)},
    {"native_getAudioPresentations", "(I)Ljava/util/List;", GESHER_NATIVE(ME_getAudioPresentations)},
};

}  // namespace

int RegisterMediaExtractor(JNIEnv* env) {
  if (RegisterStandins(env, "android/media/MediaExtractor")) {
    RegisterMethods(env, "android/media/MediaExtractor", kMethods);
  }
  return 0;
}

}  // namespace gesher::media
