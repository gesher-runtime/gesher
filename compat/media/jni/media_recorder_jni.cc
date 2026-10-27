// Gesher: android.media.MediaRecorder's natives over compat/media/record
// (docs/media.md "Recording", ADR 0059), in libmedia_jni.so. The recorder's
// events (MEDIA_RECORDER_EVENT_INFO: the maximum duration or size reached;
// _ERROR) come back through MediaRecorder.postEventFromNative. Recording
// audio needs RECORD_AUDIO in the app's manifest: setAudioSource fails
// without it, as on a phone.

#include <jni.h>
#include <unistd.h>

#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

#include "../audio/log.h"
#include "../record/recorder.h"
#include "jni_util.h"
#include "media_jni.h"

namespace gesher::media {
namespace {

struct {
  jfieldID context;  // long mNativeContext
  jclass clazz;
  jmethodID post_event;  // static postEventFromNative(Object, int, int, int, Object)
} gF;

std::mutex gLock;

struct JniRecorder {
  std::shared_ptr<GlobalRef> weak_this;
  std::unique_ptr<Recorder> recorder;
  std::unique_ptr<GlobalRef> surface;  // getSurface's Surface, the same each time
};

JniRecorder* Get(JNIEnv* env, jobject thiz) {
  auto* r = reinterpret_cast<JniRecorder*>(env->GetLongField(thiz, gF.context));
  if (r == nullptr) jniThrow(env, "java/lang/IllegalStateException", "the recorder was released");
  return r;
}

int FdOf(JNIEnv* env, jobject fileDescriptor) {
  if (fileDescriptor == nullptr) return -1;
  jclass cls = env->FindClass("java/io/FileDescriptor");
  jfieldID f = env->GetFieldID(cls, "descriptor", "I");
  return f != nullptr ? env->GetIntField(fileDescriptor, f) : -1;
}

void JNICALL MR_init(JNIEnv* env, jclass) {
  jclass cls = env->FindClass("android/media/MediaRecorder");
  if (cls == nullptr) {
    env->ExceptionClear();
    return;
  }
  gF.clazz = static_cast<jclass>(env->NewGlobalRef(cls));
  gF.context = env->GetFieldID(cls, "mNativeContext", "J");
  gF.post_event = env->GetStaticMethodID(cls, "postEventFromNative",
                                         "(Ljava/lang/Object;IIILjava/lang/Object;)V");
  CheckException(env, "MediaRecorder.native_init");
}

void JNICALL MR_setup(JNIEnv* env, jobject thiz, jobject weak_this, jstring, jobject) {
  auto* r = new JniRecorder();
  r->weak_this = std::make_shared<GlobalRef>(env, weak_this);
  std::weak_ptr<GlobalRef> weak = r->weak_this;
  r->recorder = std::make_unique<Recorder>([weak](int what, int extra) {
    auto w = weak.lock();
    if (w == nullptr) return;
    JNIEnv* e = AttachedEnv("gesher-recorder");
    if (e == nullptr) return;
    e->CallStaticVoidMethod(gF.clazz, gF.post_event, w->get(), what, extra, 0, nullptr);
    CheckException(e, "MediaRecorder.postEventFromNative");
  });
  std::lock_guard<std::mutex> lock(gLock);
  env->SetLongField(thiz, gF.context, reinterpret_cast<jlong>(r));
}

void Release(JNIEnv* env, jobject thiz) {
  JniRecorder* r;
  {
    std::lock_guard<std::mutex> lock(gLock);
    r = reinterpret_cast<JniRecorder*>(env->GetLongField(thiz, gF.context));
    env->SetLongField(thiz, gF.context, 0);
  }
  delete r;
}

void JNICALL MR_release(JNIEnv* env, jobject thiz) { Release(env, thiz); }
void JNICALL MR_finalize(JNIEnv* env, jobject thiz) { Release(env, thiz); }

void JNICALL MR_reset(JNIEnv* env, jobject thiz) {
  if (JniRecorder* r = Get(env, thiz)) {
    r->surface.reset();
    r->recorder->Reset();
  }
}

void JNICALL MR_setAudioSource(JNIEnv* env, jobject thiz, jint source) {
  JniRecorder* r = Get(env, thiz);
  if (r == nullptr) return;
  if (source < 0 || source > 10) {
    jniThrow(env, "java/lang/IllegalArgumentException", "Invalid audio source");
    return;
  }
  if (!MayRecordAudio(env)) {
    MEDIA_LOGE("MediaRecorder: the app does not have android.permission.RECORD_AUDIO");
    jniThrow(env, "java/lang/RuntimeException", "setAudioSource failed.");
    return;
  }
  r->recorder->config().audio_source = source;
}

void JNICALL MR_setVideoSource(JNIEnv* env, jobject thiz, jint source) {
  JniRecorder* r = Get(env, thiz);
  if (r == nullptr) return;
  if (source < 0 || source > 2) {
    jniThrow(env, "java/lang/IllegalArgumentException", "Invalid video source");
    return;
  }
  r->recorder->config().video_source = source;
}

#define INT_SETTER(Name, field)                                   \
  void JNICALL Name(JNIEnv* env, jobject thiz, jint v) {          \
    if (JniRecorder* r = Get(env, thiz)) r->recorder->config().field = v; \
  }
INT_SETTER(MR_setOutputFormat, output_format)
INT_SETTER(MR_setAudioEncoder, audio_encoder)
INT_SETTER(MR_setVideoEncoder, video_encoder)
INT_SETTER(MR_setVideoFrameRate, frame_rate)
INT_SETTER(MR_setMaxDuration, max_duration_ms)
#undef INT_SETTER

void JNICALL MR_setVideoSize(JNIEnv* env, jobject thiz, jint w, jint h) {
  JniRecorder* r = Get(env, thiz);
  if (r == nullptr) return;
  if (w <= 0 || h <= 0) {
    jniThrow(env, "java/lang/IllegalArgumentException", "Invalid video size");
    return;
  }
  r->recorder->config().width = w;
  r->recorder->config().height = h;
}

void JNICALL MR_setMaxFileSize(JNIEnv* env, jobject thiz, jlong size) {
  if (JniRecorder* r = Get(env, thiz)) r->recorder->config().max_file_size = size;
}

void JNICALL MR_setOutputFile(JNIEnv* env, jobject thiz, jobject fd) {
  JniRecorder* r = Get(env, thiz);
  if (r == nullptr) return;
  int f = FdOf(env, fd);
  int copy = f >= 0 ? dup(f) : -1;
  if (copy < 0) {
    jniThrow(env, "java/io/IOException", "invalid file descriptor");
    return;
  }
  RecorderConfig& c = r->recorder->config();
  if (c.fd >= 0) close(c.fd);
  c.fd = copy;
}

void JNICALL MR_setParameter(JNIEnv* env, jobject thiz, jstring pair) {
  JniRecorder* r = Get(env, thiz);
  if (r == nullptr || pair == nullptr) return;
  std::string s = Utf8(env, pair);
  size_t eq = s.find('=');
  if (eq == std::string::npos) return;
  std::string key = s.substr(0, eq);
  long long v = atoll(s.c_str() + eq + 1);
  RecorderConfig& c = r->recorder->config();
  if (key == "audio-param-sampling-rate") c.sample_rate = static_cast<int>(v);
  else if (key == "audio-param-number-of-channels") c.channels = static_cast<int>(v);
  else if (key == "audio-param-encoding-bitrate") c.audio_bitrate = static_cast<int>(v);
  else if (key == "video-param-encoding-bitrate") c.video_bitrate = static_cast<int>(v);
  else if (key == "video-param-rotation-angle-degrees") c.orientation = static_cast<int>(v);
  else if (key == "max-duration") c.max_duration_ms = v;
  else if (key == "max-filesize") c.max_file_size = v;
}

void JNICALL MR_prepare(JNIEnv* env, jobject thiz) {
  JniRecorder* r = Get(env, thiz);
  if (r == nullptr) return;
  std::string error;
  if (!r->recorder->Prepare(&error)) {
    MEDIA_LOGE("MediaRecorder.prepare: %s", error.c_str());
    jniThrow(env, "java/io/IOException", ("prepare failed: " + error).c_str());
  }
}

jobject JNICALL MR_getSurface(JNIEnv* env, jobject thiz) {
  JniRecorder* r = Get(env, thiz);
  if (r == nullptr) return nullptr;
  if (r->recorder->input_surface() == nullptr) {
    jniThrow(env, "java/lang/IllegalStateException",
             "getSurface(): no video source SURFACE, or before prepare()");
    return nullptr;
  }
  if (r->surface == nullptr) {
    jobject s = r->recorder->input_surface()->NewJavaSurface(env);
    if (s == nullptr) return nullptr;
    r->surface = std::make_unique<GlobalRef>(env, s);
    env->DeleteLocalRef(s);
  }
  return env->NewLocalRef(r->surface->get());
}

void JNICALL MR_start(JNIEnv* env, jobject thiz) {
  JniRecorder* r = Get(env, thiz);
  if (r == nullptr) return;
  std::string error;
  if (!r->recorder->Start(&error)) {
    MEDIA_LOGE("MediaRecorder.start: %s", error.c_str());
    jniThrow(env, "java/lang/IllegalStateException", error.c_str());
  }
}

void JNICALL MR_stop(JNIEnv* env, jobject thiz) {
  JniRecorder* r = Get(env, thiz);
  if (r == nullptr) return;
  std::string error;
  bool ok = r->recorder->Stop(&error);
  r->surface.reset();
  if (!ok) {
    MEDIA_LOGE("MediaRecorder.stop: %s", error.c_str());
    jniThrow(env, "java/lang/RuntimeException", "stop failed.");
  }
}

void JNICALL MR_pause(JNIEnv* env, jobject thiz) {
  if (JniRecorder* r = Get(env, thiz)) r->recorder->Pause();
}

void JNICALL MR_resume(JNIEnv* env, jobject thiz) {
  if (JniRecorder* r = Get(env, thiz)) r->recorder->Resume();
}

jint JNICALL MR_getMaxAmplitude(JNIEnv* env, jobject thiz) {
  JniRecorder* r = Get(env, thiz);
  return r != nullptr ? r->recorder->MaxAmplitude() : 0;
}

void JNICALL MR_setCamera(JNIEnv*, jobject, jobject) {
  MEDIA_LOGW("MediaRecorder.setCamera: the legacy camera's recording is not supported (use a Surface)");
}

jintArray JNICALL MR_routedDevices(JNIEnv* env, jobject) {
  jintArray a = env->NewIntArray(1);
  jint id = kInputDeviceId;
  env->SetIntArrayRegion(a, 0, 1, &id);
  return a;
}

const JNINativeMethod kMethods[] = {
    {"native_init", "()V", GESHER_NATIVE(MR_init)},
    {"native_setup", "(Ljava/lang/Object;Ljava/lang/String;Landroid/os/Parcel;)V", GESHER_NATIVE(MR_setup)},
    {"native_finalize", "()V", GESHER_NATIVE(MR_finalize)},
    {"release", "()V", GESHER_NATIVE(MR_release)},
    {"native_reset", "()V", GESHER_NATIVE(MR_reset)},
    {"setAudioSource", "(I)V", GESHER_NATIVE(MR_setAudioSource)},
    {"setVideoSource", "(I)V", GESHER_NATIVE(MR_setVideoSource)},
    {"setOutputFormat", "(I)V", GESHER_NATIVE(MR_setOutputFormat)},
    {"setAudioEncoder", "(I)V", GESHER_NATIVE(MR_setAudioEncoder)},
    {"setVideoEncoder", "(I)V", GESHER_NATIVE(MR_setVideoEncoder)},
    {"setVideoFrameRate", "(I)V", GESHER_NATIVE(MR_setVideoFrameRate)},
    {"setMaxDuration", "(I)V", GESHER_NATIVE(MR_setMaxDuration)},
    {"setVideoSize", "(II)V", GESHER_NATIVE(MR_setVideoSize)},
    {"setMaxFileSize", "(J)V", GESHER_NATIVE(MR_setMaxFileSize)},
    {"_setOutputFile", "(Ljava/io/FileDescriptor;)V", GESHER_NATIVE(MR_setOutputFile)},
    {"setParameter", "(Ljava/lang/String;)V", GESHER_NATIVE(MR_setParameter)},
    {"_prepare", "()V", GESHER_NATIVE(MR_prepare)},
    {"getSurface", "()Landroid/view/Surface;", GESHER_NATIVE(MR_getSurface)},
    {"start", "()V", GESHER_NATIVE(MR_start)},
    {"stop", "()V", GESHER_NATIVE(MR_stop)},
    {"pause", "()V", GESHER_NATIVE(MR_pause)},
    {"resume", "()V", GESHER_NATIVE(MR_resume)},
    {"getMaxAmplitude", "()I", GESHER_NATIVE(MR_getMaxAmplitude)},
    {"setCamera", "(Landroid/hardware/Camera;)V", GESHER_NATIVE(MR_setCamera)},
    {"native_getRoutedDeviceIds", "()[I", GESHER_NATIVE(MR_routedDevices)},
};

}  // namespace

int RegisterMediaRecorder(JNIEnv* env) {
  if (RegisterStandins(env, "android/media/MediaRecorder")) {
    RegisterMethods(env, "android/media/MediaRecorder", kMethods);
  }
  return 0;
}

}  // namespace gesher::media
