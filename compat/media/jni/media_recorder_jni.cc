// Gesher: android.media.MediaRecorder's natives over compat/media/record
// (docs/media.md "Recording", ADR 0059), in libmedia_jni.so. The recorder's
// events (MEDIA_RECORDER_EVENT_INFO: the maximum duration or size reached;
// _ERROR) come back through MediaRecorder.postEventFromNative. Recording
// audio needs RECORD_AUDIO in the app's manifest: setAudioSource fails
// without it, as on a phone.

#include <jni.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <cerrno>
#include <climits>
#include <memory>
#include <mutex>
#include <string>

#include "../audio/log.h"
#include "../record/recorder.h"
#include "jni_util.h"
#include "media_jni.h"
#include "receiver_lease.h"

namespace gesher::media {
namespace {

struct {
  jfieldID context;  // long mNativeContext
  jclass clazz;
  jmethodID post_event;  // static postEventFromNative(Object, int, int, int, Object)
} gF;

std::mutex gLock;

struct JniRecorder : ReceiverState {
  std::shared_ptr<GlobalRef> weak_this;
  std::unique_ptr<Recorder> recorder;
  std::unique_ptr<GlobalRef> surface;  // getSurface's Surface, the same each time

  JniRecorder(JNIEnv* env, jobject weak) : weak_this(std::make_shared<GlobalRef>(env, weak)) {
    if (env->ExceptionCheck()) return;
    std::weak_ptr<GlobalRef> callback = weak_this;
    recorder.reset(new (std::nothrow) Recorder([callback](int what, int extra) {
      auto w = callback.lock();
      if (w == nullptr) return;
      JNIEnv* e = AttachedEnv("gesher-recorder");
      if (e == nullptr) return;
      e->CallStaticVoidMethod(gF.clazz, gF.post_event, w->get(), what, extra, 0, nullptr);
      CheckException(e, "MediaRecorder.postEventFromNative");
    }));
    if (recorder == nullptr) jniThrow(env, "java/lang/OutOfMemoryError", "media recorder");
  }
};

ReceiverLease<JniRecorder> Get(JNIEnv* env, jobject thiz) {
  auto r = ReceiverLease<JniRecorder>::Acquire(env, thiz, gF.context, gLock);
  if (r == nullptr) jniThrow(env, "java/lang/IllegalStateException",
                           "the recorder was released or its operation was reentered");
  return r;
}

ReceiverLease<JniRecorder> GetConfigurable(JNIEnv* env, jobject thiz) {
  auto r = Get(env, thiz);
  if (r != nullptr && !r->recorder->CanConfigure()) {
    jniThrow(env, "java/lang/IllegalStateException", "the recorder is not configurable; reset a failed session");
    return {};
  }
  return r;
}

bool RequireState(JNIEnv* env, bool allowed, const char* operation) {
  if (!allowed) jniThrow(env, "java/lang/IllegalStateException", operation);
  return allowed;
}

bool SetDuration(JNIEnv* env, RecorderConfig& c, int64_t value) {
  if (value > 0 && value <= 100) {
    jniThrow(env, "java/lang/RuntimeException", "maximum duration must exceed 100 ms");
    return false;
  }
  c.max_duration_ms = value > 0 ? value : 0;
  return true;
}

bool SetFileSize(JNIEnv* env, RecorderConfig& c, int64_t value) {
  if (value > 0 && value <= 1024) {
    jniThrow(env, "java/lang/RuntimeException", "maximum file size must exceed 1024 bytes");
    return false;
  }
  c.max_file_size = value > 0 ? value : 0;
  return true;
}

int FdOf(JNIEnv* env, jobject fileDescriptor) {
  if (fileDescriptor == nullptr) return -1;
  jclass cls = env->FindClass("java/io/FileDescriptor");
  if (cls == nullptr) return -1;
  jfieldID f = env->GetFieldID(cls, "descriptor", "I");
  const int fd = f != nullptr ? env->GetIntField(fileDescriptor, f) : -1;
  env->DeleteLocalRef(cls);
  return fd;
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
  ReceiverLease<JniRecorder>::Setup(env, thiz, gF.context, gLock, env, weak_this);
}

void Release(JNIEnv* env, jobject thiz) {
  ReceiverLease<JniRecorder>::Release(env, thiz, gF.context, gLock);
}

void JNICALL MR_release(JNIEnv* env, jobject thiz) { Release(env, thiz); }
void JNICALL MR_finalize(JNIEnv* env, jobject thiz) { Release(env, thiz); }

void JNICALL MR_reset(JNIEnv* env, jobject thiz) {
  if (auto r = Get(env, thiz)) {
    r->surface.reset();
    r->recorder->Reset();
  }
}

void JNICALL MR_setAudioSource(JNIEnv* env, jobject thiz, jint source) {
  if (source < 0 || (source > 10 && source != 1998 /* FM_TUNER */)) {
    jniThrow(env, "java/lang/IllegalArgumentException", "Invalid audio source");
    return;
  }
  auto r = Get(env, thiz);
  if (r == nullptr || !RequireState(env, r->recorder->CanSetSource(true),
                                   "setAudioSource requires an unset source before output format")) return;
  if (!MayRecordAudio(env)) {
    MEDIA_LOGE("MediaRecorder: the app does not have android.permission.RECORD_AUDIO");
    jniThrow(env, "java/lang/RuntimeException", "setAudioSource failed.");
    r->recorder->EnterError();
    return;
  }
  if ((source >= 2 && source <= 4) || source == 8 || source == 1998) {
    // A microphone is not a telephony uplink/downlink, remote playback mix
    // or FM tuner. Those distinct sources have no capture backend here.
    r->recorder->EnterError();
    jniThrow(env, "java/lang/RuntimeException", "requested audio source is unavailable");
    return;
  }
  r->recorder->config().audio_source = source == 0 ? 1 : source;
}

void JNICALL MR_setVideoSource(JNIEnv* env, jobject thiz, jint source) {
  if (source < 0 || source > 2) {
    jniThrow(env, "java/lang/IllegalArgumentException", "Invalid video source");
    return;
  }
  auto r = Get(env, thiz);
  if (r == nullptr || !RequireState(env, r->recorder->CanSetSource(false),
                                   "setVideoSource requires an unset source before output format")) return;
  r->recorder->config().video_source = source == 0 ? 1 : source;  // AOSP DEFAULT is CAMERA.
}

void JNICALL MR_setOutputFormat(JNIEnv* env, jobject thiz, jint format) {
  if (format < 0 || format >= 12) {
    jniThrow(env, "java/lang/IllegalArgumentException", "Invalid output format");
    return;
  }
  auto r = Get(env, thiz);
  if (r == nullptr || !RequireState(env, r->recorder->CanSetOutputFormat(),
                                   "setOutputFormat requires an initialized source")) return;
  RecorderConfig& c = r->recorder->config();
  if (!RequireState(env, !(c.video_source >= 0 && format >= 3 && format < 7),
                    "audio-only output format cannot have a video source")) return;
  c.output_format = format;
}

void JNICALL MR_setAudioEncoder(JNIEnv* env, jobject thiz, jint encoder) {
  if (encoder < 0 || encoder >= 8) {
    jniThrow(env, "java/lang/IllegalArgumentException", "Invalid audio encoder");
    return;
  }
  auto r = Get(env, thiz);
  if (r == nullptr || !RequireState(env, r->recorder->CanSetEncoder(true),
                                   "setAudioEncoder requires its source and output format, once")) return;
  r->recorder->config().audio_encoder = encoder;
}

void JNICALL MR_setVideoEncoder(JNIEnv* env, jobject thiz, jint encoder) {
  if (encoder < 0 || encoder >= 10) {
    jniThrow(env, "java/lang/IllegalArgumentException", "Invalid video encoder");
    return;
  }
  auto r = Get(env, thiz);
  if (r == nullptr || !RequireState(env, r->recorder->CanSetEncoder(false),
                                   "setVideoEncoder requires its source and output format, once")) return;
  r->recorder->config().video_encoder = encoder;
}

void JNICALL MR_setVideoFrameRate(JNIEnv* env, jobject thiz, jint rate) {
  if (rate <= 0) {
    jniThrow(env, "java/lang/IllegalArgumentException", "Invalid frame rate");
    return;
  }
  auto r = Get(env, thiz);
  if (r == nullptr || !RequireState(env, r->recorder->IsConfigured() &&
                                   r->recorder->config().video_source >= 0,
                                   "setVideoFrameRate requires a configured video source")) return;
  if (rate > 1000) {  // StagefrightRecorder::kMaxHighSpeedFps, android-16.0.0_r4.
    r->recorder->EnterError();
    jniThrow(env, "java/lang/RuntimeException", "setVideoFrameRate failed");
    return;
  }
  r->recorder->config().frame_rate = rate;
}

void JNICALL MR_setMaxDuration(JNIEnv* env, jobject thiz, jint duration) {
  if (auto r = GetConfigurable(env, thiz)) SetDuration(env, r->recorder->config(), duration);
}

void JNICALL MR_setVideoSize(JNIEnv* env, jobject thiz, jint w, jint h) {
  auto r = Get(env, thiz);
  if (r == nullptr) return;
  if (w <= 0 || h <= 0) {
    jniThrow(env, "java/lang/IllegalArgumentException", "Invalid video size");
    return;
  }
  if (!RequireState(env, r->recorder->IsConfigured() && r->recorder->config().video_source >= 0,
                    "setVideoSize requires a configured video source")) return;
  r->recorder->config().width = w;
  r->recorder->config().height = h;
}

void JNICALL MR_setMaxFileSize(JNIEnv* env, jobject thiz, jlong size) {
  if (auto r = GetConfigurable(env, thiz)) SetFileSize(env, r->recorder->config(), size);
}

void JNICALL MR_setOutputFile(JNIEnv* env, jobject thiz, jobject fd) {
  if (fd == nullptr) {
    jniThrow(env, "java/lang/IllegalArgumentException", "null file descriptor");
    return;
  }
  auto r = Get(env, thiz);
  if (r == nullptr) return;
  if (!RequireState(env, r->recorder->IsConfigured() && r->recorder->config().fd < 0,
                    "setOutputFile requires output format and an unset file")) return;
  int f = FdOf(env, fd);
  if (env->ExceptionCheck()) return;
  const int flags = f >= 0 ? fcntl(f, F_GETFL) : -1;
  if (flags >= 0 && (flags & O_ACCMODE) == O_RDONLY) {
    // MediaRecorder.cpp rejects read-only descriptors before calling the
    // backend; this argument failure does not enter ERROR.
    jniThrow(env, "java/io/IOException", "output descriptor is read-only");
    return;
  }
  int copy = f >= 0 ? dup(f) : -1;
  if (copy < 0) {
    r->recorder->EnterError();
    jniThrow(env, "java/io/IOException", "invalid file descriptor");
    return;
  }
  // StagefrightRecorder::setOutputFile starts with an empty file. Without
  // this, reusing a longer descriptor retains stale trailing bytes and can
  // exceed a valid completed-file size limit even when every new write fits.
  // As in Stagefright, a non-regular descriptor is allowed here and its
  // actual seek/write failure is reported by prepare/start/stop.
  const int truncate_result = ftruncate(copy, 0);
  (void)truncate_result;
  RecorderConfig& c = r->recorder->config();
  c.fd = copy;
}

void JNICALL MR_setParameter(JNIEnv* env, jobject thiz, jstring pair) {
  if (pair == nullptr) return;
  auto r = GetConfigurable(env, thiz);
  if (r == nullptr) return;
  std::string s = Utf8(env, pair);
  if (env->ExceptionCheck()) return;
  RecorderConfig& c = r->recorder->config();
  auto apply = [&](const std::string& entry) {
    size_t eq = entry.find('=');
    if (eq == std::string::npos) {
      jniThrow(env, "java/lang/RuntimeException", "invalid recorder parameter");
      return;
    }
    std::string key = entry.substr(0, eq);
    std::string value = entry.substr(eq + 1);
    auto trim = [](std::string& text) {
      const size_t first = text.find_first_not_of(" \t\r\n\f\v");
      if (first == std::string::npos) { text.clear(); return; }
      text = text.substr(first, text.find_last_not_of(" \t\r\n\f\v") - first + 1);
    };
    trim(key);
    trim(value);
    char* end = nullptr;
    errno = 0;
    const long long v = strtoll(value.c_str(), &end, 10);
    if (value.empty() || end == value.c_str() || *end != '\0' || errno == ERANGE) {
      jniThrow(env, "java/lang/RuntimeException", "invalid recorder parameter value");
      return;
    }
    if (key == "max-duration") { SetDuration(env, c, v); return; }
    if (key == "max-filesize") { SetFileSize(env, c, v); return; }
    if (v > INT_MAX || v < 0 ||
        (key != "video-param-rotation-angle-degrees" && v == 0) ||
        (key == "audio-param-number-of-channels" && v > 2) ||
        (key == "video-param-rotation-angle-degrees" && v % 90 != 0)) {
      jniThrow(env, "java/lang/RuntimeException", "unsupported recorder parameter value");
      return;
    }
    if (key == "audio-param-sampling-rate") c.sample_rate = static_cast<int>(v);
    else if (key == "audio-param-number-of-channels") c.channels = static_cast<int>(v);
    else if (key == "audio-param-encoding-bitrate") c.audio_bitrate = static_cast<int>(v);
    else if (key == "video-param-encoding-bitrate") c.video_bitrate = static_cast<int>(v);
    else if (key == "video-param-rotation-angle-degrees") c.orientation = static_cast<int>(v % 360);
    else jniThrow(env, "java/lang/RuntimeException", "unsupported recorder parameter");
  };
  // Stagefright accepts semicolon-separated parameters in order; a later
  // invalid entry does not roll back earlier successful entries or poison
  // the recorder state.
  size_t begin = 0;
  for (;;) {
    const size_t end = s.find(';', begin);
    apply(s.substr(begin, end == std::string::npos ? end : end - begin));
    if (env->ExceptionCheck() || end == std::string::npos) return;
    begin = end + 1;
  }
}

void JNICALL MR_prepare(JNIEnv* env, jobject thiz) {
  auto r = Get(env, thiz);
  if (r == nullptr) return;
  std::string error;
  bool invalid_state = false;
  if (!r->recorder->Prepare(&error, &invalid_state)) {
    MEDIA_LOGE("MediaRecorder.prepare: %s", error.c_str());
    jniThrow(env, invalid_state ? "java/lang/IllegalStateException" : "java/io/IOException",
             ("prepare failed: " + error).c_str());
  }
}

jobject JNICALL MR_getSurface(JNIEnv* env, jobject thiz) {
  auto r = Get(env, thiz);
  if (r == nullptr) return nullptr;
  auto* input = r->recorder->input_surface();
  if (input == nullptr) {
    jniThrow(env, "java/lang/IllegalStateException",
             "getSurface(): no video source SURFACE, or before prepare()");
    return nullptr;
  }
  if (r->surface == nullptr) {
    jobject s = input->NewJavaSurface(env);
    if (s == nullptr) return nullptr;
    r->surface = std::make_unique<GlobalRef>(env, s);
    env->DeleteLocalRef(s);
  }
  return env->NewLocalRef(r->surface->get());
}

void JNICALL MR_start(JNIEnv* env, jobject thiz) {
  auto r = Get(env, thiz);
  if (r == nullptr) return;
  std::string error;
  bool invalid_state = false;
  if (!r->recorder->Start(&error, &invalid_state)) {
    MEDIA_LOGE("MediaRecorder.start: %s", error.c_str());
    jniThrow(env, invalid_state ? "java/lang/IllegalStateException" : "java/lang/RuntimeException", error.c_str());
  }
}

void JNICALL MR_stop(JNIEnv* env, jobject thiz) {
  auto r = Get(env, thiz);
  if (r == nullptr) return;
  std::string error;
  bool invalid_state = false;
  bool ok = r->recorder->Stop(&error, &invalid_state);
  if (!invalid_state) r->surface.reset();
  if (!ok) {
    MEDIA_LOGE("MediaRecorder.stop: %s", error.c_str());
    jniThrow(env, invalid_state ? "java/lang/IllegalStateException" : "java/lang/RuntimeException", error.c_str());
  }
}

void JNICALL MR_pause(JNIEnv* env, jobject thiz) {
  auto r = Get(env, thiz);
  if (r == nullptr) return;
  std::string error;
  bool invalid_state = false;
  if (!r->recorder->Pause(&error, &invalid_state)) {
    jniThrow(env, invalid_state ? "java/lang/IllegalStateException" : "java/lang/RuntimeException", error.c_str());
  }
}

void JNICALL MR_resume(JNIEnv* env, jobject thiz) {
  auto r = Get(env, thiz);
  if (r == nullptr) return;
  std::string error;
  bool invalid_state = false;
  if (!r->recorder->Resume(&error, &invalid_state)) {
    jniThrow(env, invalid_state ? "java/lang/IllegalStateException" : "java/lang/RuntimeException", error.c_str());
  }
}

jint JNICALL MR_getMaxAmplitude(JNIEnv* env, jobject thiz) {
  auto r = Get(env, thiz);
  if (r != nullptr && !RequireState(env, !r->recorder->InError(), "getMaxAmplitude() in ERROR")) return 0;
  return r != nullptr ? r->recorder->MaxAmplitude() : 0;
}

void JNICALL MR_setCamera(JNIEnv* env, jobject thiz, jobject camera) {
  if (camera == nullptr) {
    jniThrow(env, "java/lang/NullPointerException", "camera object is null");
    return;
  }
  auto r = Get(env, thiz);
  if (r == nullptr || !RequireState(env, r->recorder->IsIdle(), "setCamera requires IDLE")) return;
  r->recorder->EnterError();
  jniThrow(env, "java/lang/RuntimeException", "legacy camera recording is unavailable");
}

jintArray JNICALL MR_routedDevices(JNIEnv* env, jobject thiz) {
  auto r = Get(env, thiz);
  if (r == nullptr) return nullptr;
  if (!r->recorder->HasAudioInput()) return env->NewIntArray(0);
  jintArray a = env->NewIntArray(1);
  if (a == nullptr) return nullptr;
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
