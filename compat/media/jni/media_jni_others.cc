// Gesher: libmedia_jni's classes other than MediaPlayer (docs/media.md):
// MediaMetadataRetriever over the extractors (the container's metadata and
// the tracks'), MediaExtractor (media_extractor_jni.cc), and stand-ins for the codec, muxer, recorder and DRM classes
// until their stages (roadmap phase 8).

#include <jni.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "../audio/log.h"
#include "../player/data_source.h"
#include "../player/decoder.h"
#include "../player/extractor.h"
#include "jni_util.h"
#include "media_jni.h"

namespace gesher::media {
namespace {

// --- MediaMetadataRetriever ----------------------------------------------------------------

jfieldID gRetrieverContext;  // long mNativeContext

struct Retriever {
  std::map<int, std::string> metadata;
  std::vector<uint8_t> album_art;
};

Retriever* GetRetriever(JNIEnv* env, jobject thiz) {
  return reinterpret_cast<Retriever*>(env->GetLongField(thiz, gRetrieverContext));
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

bool Fill(Retriever* r, std::shared_ptr<DataSource> source, std::string* error) {
  int64_t size = source->Size();
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
  file.GetBuffer("album-art", &r->album_art);
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
      r->metadata[20] = std::to_string(size * 8 * 1000000 / duration);
    }
  }
  return true;
}

void SetFromSource(JNIEnv* env, jobject thiz, std::unique_ptr<DataSource> source,
                   const std::string& error) {
  Retriever* r = GetRetriever(env, thiz);
  if (r == nullptr) {
    jniThrow(env, "java/lang/IllegalStateException", "No retriever available");
    return;
  }
  std::string e = error;
  if (source == nullptr || !Fill(r, std::move(source), &e)) {
    MEDIA_LOGE("MediaMetadataRetriever: %s", e.c_str());
    jniThrow(env, "java/lang/IllegalArgumentException", "setDataSource failed");
  }
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
  if (fileDescriptor == nullptr) {
    jniThrow(env, "java/lang/IllegalArgumentException", nullptr);
    return;
  }
  jclass fdc = env->FindClass("java/io/FileDescriptor");
  jint fd = env->GetIntField(fileDescriptor, env->GetFieldID(fdc, "descriptor", "I"));
  std::string error;
  auto source = OpenFdSource(fd, offset, length >= 0x7fffffffffffffLL ? -1 : length, &error);
  SetFromSource(env, thiz, std::move(source), error);
}

jstring JNICALL MMR_extractMetadata(JNIEnv* env, jobject thiz, jint key) {
  Retriever* r = GetRetriever(env, thiz);
  if (r == nullptr) {
    jniThrow(env, "java/lang/IllegalStateException", "No retriever available");
    return nullptr;
  }
  auto it = r->metadata.find(key);
  return it != r->metadata.end() ? env->NewStringUTF(it->second.c_str()) : nullptr;
}

jbyteArray JNICALL MMR_getEmbeddedPicture(JNIEnv* env, jobject thiz, jint) {
  Retriever* r = GetRetriever(env, thiz);
  if (r == nullptr || r->album_art.empty()) return nullptr;
  jbyteArray a = env->NewByteArray(static_cast<jsize>(r->album_art.size()));
  env->SetByteArrayRegion(a, 0, static_cast<jsize>(r->album_art.size()),
                          reinterpret_cast<const jbyte*>(r->album_art.data()));
  return a;
}

void JNICALL MMR_init(JNIEnv* env, jclass) {
  jclass cls = env->FindClass("android/media/MediaMetadataRetriever");
  gRetrieverContext = env->GetFieldID(cls, "mNativeContext", "J");
}

void JNICALL MMR_setup(JNIEnv* env, jobject thiz) {
  env->SetLongField(thiz, gRetrieverContext, reinterpret_cast<jlong>(new Retriever));
}

void JNICALL MMR_release(JNIEnv* env, jobject thiz) {
  delete GetRetriever(env, thiz);
  env->SetLongField(thiz, gRetrieverContext, 0);
}

const JNINativeMethod kRetrieverMethods[] = {
    {"_setDataSource",
     "(Landroid/os/IBinder;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V",
     GESHER_NATIVE(MMR_setDataSourcePath)},
    {"_setDataSource", "(Ljava/io/FileDescriptor;JJ)V", GESHER_NATIVE(MMR_setDataSourceFD)},
    {"nativeExtractMetadata", "(I)Ljava/lang/String;", GESHER_NATIVE(MMR_extractMetadata)},
    {"getEmbeddedPicture", "(I)[B", GESHER_NATIVE(MMR_getEmbeddedPicture)},
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
  for (const char* cls : {"android/media/MediaMuxer",
                          "android/media/MediaRecorder", "android/media/MediaSync",
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
