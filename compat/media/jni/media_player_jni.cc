// Gesher: android.media.MediaPlayer's natives over the player
// (compat/media/player/player.h; docs/media.md), in the builtin library
// libmedia_jni.so (media_jni.cc).
//
// In place of frameworks/base media/jni/android_media_MediaPlayer.cpp, whose
// MediaPlayer is a client of mediaserver: the same calls, exceptions and
// events (process_media_player_call's rules: setDataSource and prepare throw,
// the others report MEDIA_ERROR), with the player in this process.

#include <jni.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "../audio/log.h"
#include "../audio/shared_state.h"
#include "../player/data_source.h"
#include "../player/player.h"
#include "jni_util.h"
#include "media_jni.h"

namespace gesher::media {
namespace {

struct Fields {
  jclass clazz;
  jfieldID context;     // long mNativeContext
  jmethodID post_event;  // static postEventFromNative(Object, int, int, int, Object)
} gFields;

std::mutex gLock;

struct JniPlayer {
  std::shared_ptr<Player> player;
  std::shared_ptr<GlobalRef> weak_this;
};

std::shared_ptr<Player> GetPlayer(JNIEnv* env, jobject thiz) {
  std::lock_guard<std::mutex> lock(gLock);
  auto* jp = reinterpret_cast<JniPlayer*>(env->GetLongField(thiz, gFields.context));
  return jp != nullptr ? jp->player : nullptr;
}

std::shared_ptr<Player> GetPlayerOrThrow(JNIEnv* env, jobject thiz) {
  auto p = GetPlayer(env, thiz);
  if (p == nullptr) jniThrow(env, "java/lang/IllegalStateException", nullptr);
  return p;
}

// process_media_player_call.
void Process(JNIEnv* env, const std::shared_ptr<Player>& mp, int status, const char* exception,
             const char* message) {
  if (exception == nullptr) {
    if (status != kPlayerOk && mp != nullptr) mp->ReportError(status);
    return;
  }
  if (status == kPlayerOk) return;
  if (status == kPlayerInvalidOperation) {
    jniThrow(env, "java/lang/IllegalStateException", nullptr);
  } else if (status == kPlayerBadValue) {
    jniThrow(env, "java/lang/IllegalArgumentException", nullptr);
  } else {
    char msg[256];
    snprintf(msg, sizeof(msg), "%s: status=0x%X", message, static_cast<unsigned>(status));
    jniThrow(env, exception, msg);
  }
}

// --- MediaDataSource ---------------------------------------------------------------------

class JavaDataSource : public DataSource {
 public:
  JavaDataSource(JNIEnv* env, jobject source) : source_(std::make_shared<GlobalRef>(env, source)) {
    jclass cls = env->FindClass("android/media/MediaDataSource");
    read_at_ = env->GetMethodID(cls, "readAt", "(J[BII)I");
    get_size_ = env->GetMethodID(cls, "getSize", "()J");
    close_ = env->GetMethodID(cls, "close", "()V");
    buffer_ = std::make_shared<GlobalRef>(env, env->NewByteArray(kBuffer));
  }
  ~JavaDataSource() override {
    if (JNIEnv* env = AttachedEnv("gesher-media")) {
      env->CallVoidMethod(source_->get(), close_);
      CheckException(env, "MediaDataSource.close");
    }
  }

  ssize_t ReadAt(int64_t offset, void* data, size_t size) override {
    JNIEnv* env = AttachedEnv("gesher-media");
    if (env == nullptr) return -1;
    size_t done = 0;
    while (done < size) {
      jint n = env->CallIntMethod(source_->get(), read_at_, static_cast<jlong>(offset + done),
                                  static_cast<jbyteArray>(buffer_->get()), 0,
                                  static_cast<jint>(std::min<size_t>(size - done, kBuffer)));
      if (CheckException(env, "MediaDataSource.readAt")) return -1;
      if (n <= 0) break;
      env->GetByteArrayRegion(static_cast<jbyteArray>(buffer_->get()), 0, n,
                              reinterpret_cast<jbyte*>(static_cast<uint8_t*>(data) + done));
      done += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(done);
  }

  int64_t Size() override {
    JNIEnv* env = AttachedEnv("gesher-media");
    if (env == nullptr) return -1;
    jlong s = env->CallLongMethod(source_->get(), get_size_);
    if (CheckException(env, "MediaDataSource.getSize")) return -1;
    return s;
  }

 private:
  static constexpr jint kBuffer = 64 * 1024;
  std::shared_ptr<GlobalRef> source_;
  std::shared_ptr<GlobalRef> buffer_;
  jmethodID read_at_, get_size_, close_;
};

// --- Natives -----------------------------------------------------------------------------

void JNICALL MP_native_init(JNIEnv* env, jclass) {
  jclass cls = env->FindClass("android/media/MediaPlayer");
  gFields.clazz = static_cast<jclass>(env->NewGlobalRef(cls));
  gFields.context = env->GetFieldID(cls, "mNativeContext", "J");
  gFields.post_event = env->GetStaticMethodID(cls, "postEventFromNative",
                                              "(Ljava/lang/Object;IIILjava/lang/Object;)V");
}

void JNICALL MP_native_setup(JNIEnv* env, jobject thiz, jobject weak_this, jobject /*attribution*/,
                             jint session) {
  EnsureAppName(env);
  auto jp = new JniPlayer{std::make_shared<Player>(), std::make_shared<GlobalRef>(env, weak_this)};
  jp->player->set_session_id(session != 0 ? session : NewSessionId());
  std::shared_ptr<GlobalRef> weak = jp->weak_this;
  jp->player->SetListener([weak](int what, int a, int b) {
    JNIEnv* e = AttachedEnv("gesher-media-cb");
    if (e == nullptr) return;
    e->CallStaticVoidMethod(gFields.clazz, gFields.post_event, weak->get(), what, a, b, nullptr);
    CheckException(e, "MediaPlayer.postEventFromNative");
  });
  std::lock_guard<std::mutex> lock(gLock);
  env->SetLongField(thiz, gFields.context, reinterpret_cast<jlong>(jp));
}

void ReleasePlayer(JNIEnv* env, jobject thiz) {
  JniPlayer* jp;
  {
    std::lock_guard<std::mutex> lock(gLock);
    jp = reinterpret_cast<JniPlayer*>(env->GetLongField(thiz, gFields.context));
    env->SetLongField(thiz, gFields.context, 0);
  }
  if (jp == nullptr) return;
  jp->player->Release();
  delete jp;
}

void JNICALL MP_release(JNIEnv* env, jobject thiz) { ReleasePlayer(env, thiz); }
void JNICALL MP_native_finalize(JNIEnv* env, jobject thiz) { ReleasePlayer(env, thiz); }

void JNICALL MP_setDataSourcePath(JNIEnv* env, jobject thiz, jobject /*httpService*/, jstring path,
                                  jobjectArray keys, jobjectArray values) {
  auto mp = GetPlayerOrThrow(env, thiz);
  if (mp == nullptr) return;
  if (path == nullptr) {
    jniThrow(env, "java/lang/IllegalArgumentException", nullptr);
    return;
  }
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
  std::unique_ptr<DataSource> source;
  if (p.rfind("http://", 0) == 0 || p.rfind("https://", 0) == 0) {
    source = OpenHttpSource(p, headers, &error);
  } else if (p.rfind("file://", 0) == 0 || p.find("://") == std::string::npos) {
    source = OpenFileSource(p, &error);
  } else {
    error = "unsupported URI " + p;
  }
  if (source == nullptr) {
    MEDIA_LOGE("MediaPlayer.setDataSource: %s", error.c_str());
    Process(env, mp, kMediaErrorUnsupported, "java/io/IOException", "setDataSource failed.");
    return;
  }
  Process(env, mp, mp->SetDataSource(std::move(source)), "java/io/IOException",
          "setDataSource failed.");
}

void JNICALL MP_setDataSourceFD(JNIEnv* env, jobject thiz, jobject fileDescriptor, jlong offset,
                                jlong length) {
  auto mp = GetPlayerOrThrow(env, thiz);
  if (mp == nullptr) return;
  if (fileDescriptor == nullptr) {
    jniThrow(env, "java/lang/IllegalArgumentException", nullptr);
    return;
  }
  jclass fdc = env->FindClass("java/io/FileDescriptor");
  jint fd = env->GetIntField(fileDescriptor, env->GetFieldID(fdc, "descriptor", "I"));
  std::string error;
  auto source = OpenFdSource(fd, offset, length, &error);
  if (source == nullptr) {
    MEDIA_LOGE("MediaPlayer.setDataSource(fd %d): %s", fd, error.c_str());
    Process(env, mp, kPlayerBadValue, "java/io/IOException", "setDataSourceFD failed.");
    return;
  }
  Process(env, mp, mp->SetDataSource(std::move(source)), "java/io/IOException",
          "setDataSourceFD failed.");
}

void JNICALL MP_setDataSourceCallback(JNIEnv* env, jobject thiz, jobject dataSource) {
  auto mp = GetPlayerOrThrow(env, thiz);
  if (mp == nullptr) return;
  if (dataSource == nullptr) {
    jniThrow(env, "java/lang/IllegalArgumentException", nullptr);
    return;
  }
  Process(env, mp, mp->SetDataSource(std::make_shared<JavaDataSource>(env, dataSource)),
          "java/lang/RuntimeException", "setDataSourceCallback failed.");
}

jint JNICALL MP_prepare(JNIEnv* env, jobject thiz, jobject) {
  auto mp = GetPlayerOrThrow(env, thiz);
  if (mp == nullptr) return kPlayerUnknownError;
  Process(env, mp, mp->Prepare(), "java/io/IOException", "Prepare failed.");
  return env->ExceptionCheck() ? kPlayerUnknownError : 0;
}

jint JNICALL MP_prepareAsync(JNIEnv* env, jobject thiz, jobject) {
  auto mp = GetPlayerOrThrow(env, thiz);
  if (mp == nullptr) return kPlayerUnknownError;
  Process(env, mp, mp->PrepareAsync(), "java/io/IOException", "Prepare Async failed.");
  return env->ExceptionCheck() ? kPlayerUnknownError : 0;
}

void JNICALL MP_start(JNIEnv* env, jobject thiz) {
  if (auto mp = GetPlayerOrThrow(env, thiz)) Process(env, mp, mp->Start(), nullptr, nullptr);
}
void JNICALL MP_stop(JNIEnv* env, jobject thiz) {
  if (auto mp = GetPlayerOrThrow(env, thiz)) Process(env, mp, mp->Stop(), nullptr, nullptr);
}
void JNICALL MP_pause(JNIEnv* env, jobject thiz) {
  if (auto mp = GetPlayerOrThrow(env, thiz)) Process(env, mp, mp->Pause(), nullptr, nullptr);
}
jboolean JNICALL MP_isPlaying(JNIEnv* env, jobject thiz) {
  auto mp = GetPlayerOrThrow(env, thiz);
  return mp != nullptr && mp->IsPlaying() ? JNI_TRUE : JNI_FALSE;
}
void JNICALL MP_seekTo(JNIEnv* env, jobject thiz, jlong msec, jint mode) {
  if (auto mp = GetPlayerOrThrow(env, thiz)) {
    Process(env, mp, mp->SeekTo(static_cast<int>(msec), mode), nullptr, nullptr);
  }
}
jint JNICALL MP_getCurrentPosition(JNIEnv* env, jobject thiz) {
  auto mp = GetPlayerOrThrow(env, thiz);
  if (mp == nullptr) return 0;
  int msec = 0;
  Process(env, mp, mp->GetCurrentPosition(&msec), nullptr, nullptr);
  return msec;
}
jint JNICALL MP_getDuration(JNIEnv* env, jobject thiz) {
  auto mp = GetPlayerOrThrow(env, thiz);
  if (mp == nullptr) return 0;
  int msec = 0;
  Process(env, mp, mp->GetDuration(&msec), nullptr, nullptr);
  return msec;
}
void JNICALL MP_reset(JNIEnv* env, jobject thiz) {
  if (auto mp = GetPlayerOrThrow(env, thiz)) Process(env, mp, mp->Reset(), nullptr, nullptr);
}
void JNICALL MP_setLooping(JNIEnv* env, jobject thiz, jboolean loop) {
  if (auto mp = GetPlayerOrThrow(env, thiz)) Process(env, mp, mp->SetLooping(loop), nullptr, nullptr);
}
jboolean JNICALL MP_isLooping(JNIEnv* env, jobject thiz) {
  auto mp = GetPlayerOrThrow(env, thiz);
  return mp != nullptr && mp->IsLooping() ? JNI_TRUE : JNI_FALSE;
}
void JNICALL MP_setVolume(JNIEnv* env, jobject thiz, jfloat left, jfloat right) {
  if (auto mp = GetPlayerOrThrow(env, thiz)) Process(env, mp, mp->SetVolume(left, right), nullptr, nullptr);
}
void JNICALL MP_setAudioStreamType(JNIEnv* env, jobject thiz, jint stream) {
  if (auto mp = GetPlayerOrThrow(env, thiz)) Process(env, mp, mp->SetAudioStreamType(stream), nullptr, nullptr);
}
jint JNICALL MP_getAudioStreamType(JNIEnv* env, jobject thiz) {
  auto mp = GetPlayerOrThrow(env, thiz);
  int s = 3;
  if (mp != nullptr) Process(env, mp, mp->GetAudioStreamType(&s), nullptr, nullptr);
  return s;
}

// setParameter(KEY_PARAMETER_AUDIO_ATTRIBUTES, parcel): the attributes are
// PlayerBase's mAttributes by then (setAudioAttributes stores them first).
jboolean JNICALL MP_setParameter(JNIEnv* env, jobject thiz, jint key, jobject) {
  auto mp = GetPlayerOrThrow(env, thiz);
  if (mp == nullptr) return JNI_FALSE;
  if (key == 1400) {
    jclass base = env->FindClass("android/media/PlayerBase");
    jfieldID f = env->GetFieldID(base, "mAttributes", "Landroid/media/AudioAttributes;");
    if (f == nullptr) {
      env->ExceptionClear();
      return JNI_FALSE;
    }
    jobject attrs = env->GetObjectField(thiz, f);
    return mp->SetAudioStreamType(LegacyStreamType(env, attrs)) == kPlayerOk ? JNI_TRUE : JNI_FALSE;
  }
  return JNI_TRUE;
}

jint JNICALL MP_getAudioSessionId(JNIEnv* env, jobject thiz) {
  auto mp = GetPlayerOrThrow(env, thiz);
  return mp != nullptr ? mp->session_id() : 0;
}
void JNICALL MP_setAudioSessionId(JNIEnv* env, jobject thiz, jint id) {
  if (auto mp = GetPlayerOrThrow(env, thiz)) mp->set_session_id(id);
}

void JNICALL MP_setPlaybackParams(JNIEnv* env, jobject thiz, jobject params) {
  auto mp = GetPlayerOrThrow(env, thiz);
  if (mp == nullptr || params == nullptr) return;
  jclass cls = env->GetObjectClass(params);
  jfieldID set = env->GetFieldID(cls, "mSet", "I");
  jfieldID speed = env->GetFieldID(cls, "mSpeed", "F");
  if (set == nullptr || speed == nullptr) {
    env->ExceptionClear();
    return;
  }
  if ((env->GetIntField(params, set) & 1) != 0) {
    float s = env->GetFloatField(params, speed);
    mp->SetSpeed(s);
    // Speed 0 pauses (MediaPlayer.setPlaybackParams' rule), above 0 plays.
    if (s == 0) mp->Pause();
  }
}

jobject JNICALL MP_getPlaybackParams(JNIEnv* env, jobject thiz) {
  auto mp = GetPlayerOrThrow(env, thiz);
  if (mp == nullptr) return nullptr;
  jclass cls = env->FindClass("android/media/PlaybackParams");
  jobject p = env->NewObject(cls, env->GetMethodID(cls, "<init>", "()V"));
  env->DeleteLocalRef(env->CallObjectMethod(
      p, env->GetMethodID(cls, "setSpeed", "(F)Landroid/media/PlaybackParams;"), mp->speed()));
  env->DeleteLocalRef(env->CallObjectMethod(
      p, env->GetMethodID(cls, "setPitch", "(F)Landroid/media/PlaybackParams;"), mp->speed()));
  return p;
}

jobject JNICALL MP_getMetrics(JNIEnv* env, jobject) {
  jclass cls = env->FindClass("android/os/PersistableBundle");
  return env->NewObject(cls, env->GetMethodID(cls, "<init>", "()V"));
}

jint JNICALL MP_applyVolumeShaper(JNIEnv*, jobject, jobject, jobject) {
  static std::atomic<int> next{1};
  return next++;
}

jintArray JNICALL MP_getRoutedDeviceIds(JNIEnv* env, jobject) {
  jintArray a = env->NewIntArray(1);
  jint id = kOutputDeviceId;
  env->SetIntArrayRegion(a, 0, 1, &id);
  return a;
}

jboolean JNICALL MP_setOutputDevice(JNIEnv*, jobject, jint) { return JNI_TRUE; }

const JNINativeMethod kMethods[] = {
    {"nativeSetDataSource",
     "(Landroid/os/IBinder;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V",
     GESHER_NATIVE(MP_setDataSourcePath)},
    {"_setDataSource", "(Ljava/io/FileDescriptor;JJ)V", GESHER_NATIVE(MP_setDataSourceFD)},
    {"_setDataSource", "(Landroid/media/MediaDataSource;)V", GESHER_NATIVE(MP_setDataSourceCallback)},
    {"_prepare", "(Landroid/os/Parcel;)I", GESHER_NATIVE(MP_prepare)},
    {"_prepareAsync", "(Landroid/os/Parcel;)I", GESHER_NATIVE(MP_prepareAsync)},
    {"_start", "()V", GESHER_NATIVE(MP_start)},
    {"_stop", "()V", GESHER_NATIVE(MP_stop)},
    {"native_getMetrics", "()Landroid/os/PersistableBundle;", GESHER_NATIVE(MP_getMetrics)},
    {"setPlaybackParams", "(Landroid/media/PlaybackParams;)V", GESHER_NATIVE(MP_setPlaybackParams)},
    {"getPlaybackParams", "()Landroid/media/PlaybackParams;", GESHER_NATIVE(MP_getPlaybackParams)},
    {"_seekTo", "(JI)V", GESHER_NATIVE(MP_seekTo)},
    {"_pause", "()V", GESHER_NATIVE(MP_pause)},
    {"isPlaying", "()Z", GESHER_NATIVE(MP_isPlaying)},
    {"getCurrentPosition", "()I", GESHER_NATIVE(MP_getCurrentPosition)},
    {"getDuration", "()I", GESHER_NATIVE(MP_getDuration)},
    {"_release", "()V", GESHER_NATIVE(MP_release)},
    {"_reset", "()V", GESHER_NATIVE(MP_reset)},
    {"_setAudioStreamType", "(I)V", GESHER_NATIVE(MP_setAudioStreamType)},
    {"_getAudioStreamType", "()I", GESHER_NATIVE(MP_getAudioStreamType)},
    {"setParameter", "(ILandroid/os/Parcel;)Z", GESHER_NATIVE(MP_setParameter)},
    {"setLooping", "(Z)V", GESHER_NATIVE(MP_setLooping)},
    {"isLooping", "()Z", GESHER_NATIVE(MP_isLooping)},
    {"_setVolume", "(FF)V", GESHER_NATIVE(MP_setVolume)},
    {"native_init", "()V", GESHER_NATIVE(MP_native_init)},
    {"native_setup", "(Ljava/lang/Object;Landroid/os/Parcel;I)V", GESHER_NATIVE(MP_native_setup)},
    {"native_finalize", "()V", GESHER_NATIVE(MP_native_finalize)},
    {"getAudioSessionId", "()I", GESHER_NATIVE(MP_getAudioSessionId)},
    {"native_setAudioSessionId", "(I)V", GESHER_NATIVE(MP_setAudioSessionId)},
    {"native_applyVolumeShaper",
     "(Landroid/media/VolumeShaper$Configuration;Landroid/media/VolumeShaper$Operation;)I",
     GESHER_NATIVE(MP_applyVolumeShaper)},
    {"native_setOutputDevice", "(I)Z", GESHER_NATIVE(MP_setOutputDevice)},
    {"native_getRoutedDeviceIds", "()[I", GESHER_NATIVE(MP_getRoutedDeviceIds)},
};

}  // namespace

int RegisterMediaPlayer(JNIEnv* env) {
  const char* name = "android/media/MediaPlayer";
  if (!RegisterStandins(env, name)) return 0;
  RegisterMethods(env, name, kMethods);
  return 0;
}

}  // namespace gesher::media
