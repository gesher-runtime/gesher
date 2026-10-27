// Gesher: android.media.MediaMuxer's natives over compat/media/mux (docs/media.md
// "Recording", ADR 0059), in libmedia_jni.so.

#include <jni.h>

#include <media/NdkMediaFormat.h>

#include <memory>
#include <string>

#include "../audio/log.h"
#include "../mux/muxer.h"
#include "jni_util.h"
#include "media_jni.h"

namespace gesher::media {

// MediaCodec's (media_codec_jni.cc): a MediaFormat's keys and values as an AMediaFormat.
Format FormatFromJavaArrays(JNIEnv* env, jobjectArray keys, jobjectArray values);

namespace {

int FdOf(JNIEnv* env, jobject fileDescriptor) {
  if (fileDescriptor == nullptr) return -1;
  jclass cls = env->FindClass("java/io/FileDescriptor");
  jfieldID f = env->GetFieldID(cls, "descriptor", "I");
  return f != nullptr ? env->GetIntField(fileDescriptor, f) : -1;
}

Muxer* Get(jlong p) { return reinterpret_cast<Muxer*>(p); }

jlong JNICALL MM_setup(JNIEnv* env, jclass, jobject fd, jint format) {
  MuxerFormat f;
  switch (format) {
    case 0: f = MuxerFormat::kMpeg4; break;
    case 1: f = MuxerFormat::kWebm; break;
    case 2: f = MuxerFormat::kThreeGpp; break;
    default:
      jniThrow(env, "java/lang/IllegalArgumentException", "output format not supported on Gesher");
      return 0;
  }
  std::string error;
  auto m = Muxer::Create(FdOf(env, fd), f, &error);
  if (m == nullptr) {
    jniThrow(env, "java/io/IOException", error.c_str());
    return 0;
  }
  return reinterpret_cast<jlong>(m.release());
}

void JNICALL MM_release(JNIEnv*, jclass, jlong p) { delete Get(p); }

void JNICALL MM_start(JNIEnv* env, jclass, jlong p) {
  std::string error;
  if (p == 0 || !Get(p)->Start(&error)) jniThrow(env, "java/lang/IllegalStateException", error.c_str());
}

void JNICALL MM_stop(JNIEnv* env, jclass, jlong p) {
  std::string error;
  if (p == 0 || !Get(p)->Stop(&error)) {
    MEDIA_LOGE("MediaMuxer.stop: %s", error.c_str());
    jniThrow(env, "java/lang/IllegalStateException", error.c_str());
  }
}

jint JNICALL MM_addTrack(JNIEnv* env, jclass, jlong p, jobjectArray keys, jobjectArray values) {
  if (p == 0) return -1;
  std::string error;
  int t = Get(p)->AddTrack(FormatFromJavaArrays(env, keys, values), &error);
  if (t < 0) jniThrow(env, "java/lang/IllegalArgumentException", error.c_str());
  return t;
}

void JNICALL MM_setOrientation(JNIEnv*, jclass, jlong p, jint degrees) {
  if (p != 0) Get(p)->SetOrientationHint(degrees);
}

void JNICALL MM_setLocation(JNIEnv*, jclass, jlong p, jint lat, jint lon) {
  if (p != 0) Get(p)->SetLocation(lat, lon);
}

void JNICALL MM_writeSample(JNIEnv* env, jclass, jlong p, jint track, jobject buffer, jint offset,
                            jint size, jlong time_us, jint flags) {
  if (p == 0) return;
  auto* base = static_cast<uint8_t*>(env->GetDirectBufferAddress(buffer));
  std::vector<uint8_t> copy;
  if (base == nullptr) {
    // A heap buffer: its array.
    jclass bb = env->FindClass("java/nio/ByteBuffer");
    auto arr = static_cast<jbyteArray>(env->CallObjectMethod(buffer, env->GetMethodID(bb, "array", "()[B")));
    jint array_offset = env->CallIntMethod(buffer, env->GetMethodID(bb, "arrayOffset", "()I"));
    if (env->ExceptionCheck() || arr == nullptr) return;
    copy.resize(static_cast<size_t>(size));
    env->GetByteArrayRegion(arr, array_offset + offset, size, reinterpret_cast<jbyte*>(copy.data()));
    base = copy.data();
    offset = 0;
  }
  std::string error;
  if (!Get(p)->WriteSample(track, base + offset, static_cast<size_t>(size), time_us, flags, &error)) {
    jniThrow(env, "java/lang/IllegalStateException", error.c_str());
  }
}

const JNINativeMethod kMethods[] = {
    {"nativeSetup", "(Ljava/io/FileDescriptor;I)J", GESHER_NATIVE(MM_setup)},
    {"nativeRelease", "(J)V", GESHER_NATIVE(MM_release)},
    {"nativeStart", "(J)V", GESHER_NATIVE(MM_start)},
    {"nativeStop", "(J)V", GESHER_NATIVE(MM_stop)},
    {"nativeAddTrack", "(J[Ljava/lang/String;[Ljava/lang/Object;)I", GESHER_NATIVE(MM_addTrack)},
    {"nativeSetOrientationHint", "(JI)V", GESHER_NATIVE(MM_setOrientation)},
    {"nativeSetLocation", "(JII)V", GESHER_NATIVE(MM_setLocation)},
    {"nativeWriteSampleData", "(JILjava/nio/ByteBuffer;IIJI)V", GESHER_NATIVE(MM_writeSample)},
};

}  // namespace

int RegisterMediaMuxer(JNIEnv* env) {
  if (RegisterStandins(env, "android/media/MediaMuxer")) {
    RegisterMethods(env, "android/media/MediaMuxer", kMethods);
  }
  return 0;
}

}  // namespace gesher::media
