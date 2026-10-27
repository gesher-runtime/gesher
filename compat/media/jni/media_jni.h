// Gesher media: what the media natives share (docs/media.md).
#pragma once

#include <jni.h>

#include "jni_util.h"

namespace gesher::media {

// A global reference, deleted (on whatever thread) with the last owner.
class GlobalRef {
 public:
  GlobalRef(JNIEnv* env, jobject obj) : obj_(obj != nullptr ? env->NewGlobalRef(obj) : nullptr) {}
  ~GlobalRef() {
    if (obj_ == nullptr) return;
    if (JNIEnv* env = AttachedEnv("gesher-media")) env->DeleteGlobalRef(obj_);
  }
  GlobalRef(const GlobalRef&) = delete;
  GlobalRef& operator=(const GlobalRef&) = delete;
  jobject get() const { return obj_; }

 private:
  jobject obj_;
};

// A weak global reference, deleted with the last owner.
class WeakRef {
 public:
  WeakRef(JNIEnv* env, jobject obj) : obj_(env->NewWeakGlobalRef(obj)) {}
  ~WeakRef() {
    if (JNIEnv* env = AttachedEnv("gesher-media")) env->DeleteWeakGlobalRef(obj_);
  }
  WeakRef(const WeakRef&) = delete;
  WeakRef& operator=(const WeakRef&) = delete;
  jweak get() const { return obj_; }

 private:
  jweak obj_;
};

// Throws a new `className`(message).
void jniThrow(JNIEnv* env, const char* className, const char* message);

// The id of the output device, as AudioDeviceInfo.getId() and routing show it.
constexpr int kOutputDeviceId = 2;
// The input device's (media3, ADR 0059).
constexpr int kInputDeviceId = 3;

// Whether this process's app may record audio: its manifest asks for RECORD_AUDIO
// (com.gesher.server.AudioService.recordAudioAllowed; audio_record_jni.cc). `env` may be
// null (the calling thread is attached).
bool MayRecordAudio(JNIEnv* env);

// The Windows audio sessions' name: the application's label, looked up once
// through ActivityThread (from a thread that runs Java).
void EnsureAppName(JNIEnv* env);

// AudioAttributes.toLegacyStreamType(attributes), STREAM_MUSIC if that fails.
int LegacyStreamType(JNIEnv* env, jobject attributes);

// Registrations (stand-ins first, then the natives Gesher implements).
int RegisterAudioTrack(JNIEnv* env);
int RegisterAudioSystem(JNIEnv* env);
int RegisterAudioRecord(JNIEnv* env);
int RegisterMediaPlayer(JNIEnv* env);
int RegisterSoundPool(JNIEnv* env);
// libmedia_jni's other classes: MediaMetadataRetriever, MediaExtractor,
// MediaCodec, ... (media_jni_others.cc).
int RegisterMediaJniOthers(JNIEnv* env);
int RegisterMediaExtractor(JNIEnv* env);
// MediaCodec and MediaCodecList (media_codec_jni.cc).
int RegisterMediaCodec(JNIEnv* env);
// MediaMuxer (media_muxer_jni.cc) and MediaRecorder (media_recorder_jni.cc).
int RegisterMediaMuxer(JNIEnv* env);
int RegisterMediaRecorder(JNIEnv* env);

// An AMediaFormat as the HashMap MediaFormat(Map) takes (media_extractor_jni.cc):
// Integer, Long, Float, String and ByteBuffer values.
class Format;
jobject FormatToMap(JNIEnv* env, const Format& f);

}  // namespace gesher::media

// libandroid_runtime's row (compat/framework/android_runtime.cc) calls
// android::register_gesher_media (audio_system_jni.cc).
