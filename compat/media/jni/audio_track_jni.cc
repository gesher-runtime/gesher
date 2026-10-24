// Gesher: android.media.AudioTrack's natives over the engine (docs/media.md).
//
// In place of frameworks/base core/jni/android_media_AudioTrack.cpp, which
// drives libaudioclient's AudioTrack against AudioFlinger: here each Java
// AudioTrack owns an engine Track (compat/media/audio/engine.h), mixed in this
// process. Markers and periodic positions come back through
// AudioTrack.postEventFromNative, from the engine's event thread.

#include <jni.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../audio/engine.h"
#include "../audio/log.h"
#include "../audio/shared_state.h"
#include "jni_util.h"
#include "media_jni.h"

namespace gesher::media {
namespace {

// android.media.AudioFormat encodings.
constexpr int kEncodingPcm16 = 2;
constexpr int kEncodingPcm8 = 3;
constexpr int kEncodingPcmFloat = 4;
constexpr int kEncodingPcm24Packed = 21;
constexpr int kEncodingPcm32 = 22;
constexpr int kEncodingDefault = 1;
// AudioTrack.MODE_STATIC.
constexpr int kModeStatic = 0;
// AudioTrack's AUDIOTRACK_ERROR_SETUP_* codes.
constexpr int kSetupInvalidChannelMask = -17;
constexpr int kSetupInvalidFormat = -18;
constexpr int kSetupNativeInitFailed = -20;
// AudioTrack's native events.
constexpr int kEventMarker = 3;
constexpr int kEventNewPos = 4;

struct Fields {
  jfieldID native_track;  // long mNativeTrackInJavaObj
  jfieldID jni_data;      // long mJniData
  jfieldID stream_type;   // int mStreamType
  jclass clazz;
  jmethodID post_event;   // static postEventFromNative(Object, int, int, int, Object)
} gFields;

std::mutex gLock;  // the Java objects' pointers, as android_media_AudioTrack's sLock

struct JniTrack {
  std::shared_ptr<Track> track;
  std::shared_ptr<GlobalRef> weak_this;  // the WeakReference<AudioTrack>
};

PcmFormat ToPcm(int encoding, bool* ok) {
  *ok = true;
  switch (encoding) {
    case kEncodingPcm8: return PcmFormat::kU8;
    case kEncodingPcm16:
    case kEncodingDefault: return PcmFormat::kS16;
    case kEncodingPcmFloat: return PcmFormat::kFloat;
    case kEncodingPcm24Packed: return PcmFormat::kS24Packed;
    case kEncodingPcm32: return PcmFormat::kS32;
  }
  *ok = false;
  return PcmFormat::kS16;
}

std::shared_ptr<Track> GetTrack(JNIEnv* env, jobject thiz) {
  std::lock_guard<std::mutex> lock(gLock);
  auto* jt = reinterpret_cast<JniTrack*>(env->GetLongField(thiz, gFields.jni_data));
  return jt != nullptr ? jt->track : nullptr;
}

std::shared_ptr<Track> GetTrackOrThrow(JNIEnv* env, jobject thiz, const char* what) {
  auto t = GetTrack(env, thiz);
  if (t == nullptr) {
    std::string msg = std::string("Unable to retrieve AudioTrack pointer for ") + what;
    jniThrow(env, "java/lang/IllegalStateException", msg.c_str());
  }
  return t;
}

void PostToJava(const std::shared_ptr<GlobalRef>& weak, int what, int arg1, int arg2) {
  JNIEnv* env = AttachedEnv("gesher-audio-cb");
  if (env == nullptr) return;
  env->CallStaticVoidMethod(gFields.clazz, gFields.post_event, weak->get(), what, arg1, arg2,
                            nullptr);
  CheckException(env, "AudioTrack.postEventFromNative");
}

jint JNICALL AudioTrack_setup(JNIEnv* env, jobject thiz, jobject weak_this, jobject attributes,
                              jintArray jSampleRate, jint channelPositionMask, jint channelIndexMask,
                              jint audioFormat, jint buffSizeInBytes, jint memoryMode,
                              jintArray jSession, jobject /*attributionSource*/,
                              jlong nativeAudioTrack, jboolean offload, jint /*encapsulationMode*/,
                              jobject /*tunerConfiguration*/, jstring /*opPackageName*/) {
  if (nativeAudioTrack != 0 || offload) {
    MEDIA_LOGW("AudioTrack: %s tracks are not supported", offload ? "offloaded" : "native");
    return kSetupNativeInitFailed;
  }
  if (jSampleRate == nullptr || jSession == nullptr) return kSetupNativeInitFailed;
  EnsureAppName(env);
  int channels = __builtin_popcount(static_cast<unsigned>(channelPositionMask)) +
                 (channelPositionMask == 0 ? __builtin_popcount(static_cast<unsigned>(channelIndexMask)) : 0);
  if (channels <= 0 || channels > 24) {
    MEDIA_LOGE("AudioTrack: channel mask 0x%x, index mask 0x%x", channelPositionMask, channelIndexMask);
    return kSetupInvalidChannelMask;
  }
  bool ok;
  PcmFormat format = ToPcm(audioFormat, &ok);
  if (!ok) {
    MEDIA_LOGW("AudioTrack: encoding %d is not supported (PCM only)", audioFormat);
    return kSetupInvalidFormat;
  }
  jint rate = 0;
  env->GetIntArrayRegion(jSampleRate, 0, 1, &rate);
  if (rate == 0) {
    rate = static_cast<jint>(GetOutputInfo().sample_rate);
    env->SetIntArrayRegion(jSampleRate, 0, 1, &rate);
  }
  jint session = 0;
  env->GetIntArrayRegion(jSession, 0, 1, &session);
  if (session == 0) {
    session = NewSessionId();
    env->SetIntArrayRegion(jSession, 0, 1, &session);
  }
  int stream = LegacyStreamType(env, attributes);
  env->SetIntField(thiz, gFields.stream_type, stream);

  TrackParams p;
  p.sample_rate = static_cast<uint32_t>(rate);
  p.channels = static_cast<uint32_t>(channels);
  p.format = format;
  p.stream_type = stream;
  p.is_static = memoryMode == kModeStatic;
  size_t frame = BytesPerSample(format) * static_cast<size_t>(channels);
  p.capacity_frames = static_cast<size_t>(std::max(buffSizeInBytes, 0)) / frame;
  p.session_id = session;
  p.name = "AudioTrack";
  std::string error;
  auto track = CreateTrack(p, &error);
  if (track == nullptr) {
    MEDIA_LOGE("AudioTrack: %s", error.c_str());
    return kSetupNativeInitFailed;
  }
  auto weak = std::make_shared<GlobalRef>(env, weak_this);
  TrackEvents ev;
  ev.marker = [weak](uint32_t pos) { PostToJava(weak, kEventMarker, static_cast<int>(pos), 0); };
  ev.new_position = [weak](uint32_t pos) { PostToJava(weak, kEventNewPos, static_cast<int>(pos), 0); };
  track->SetEvents(std::move(ev));

  auto* jt = new JniTrack{track, weak};
  std::lock_guard<std::mutex> lock(gLock);
  env->SetLongField(thiz, gFields.jni_data, reinterpret_cast<jlong>(jt));
  env->SetLongField(thiz, gFields.native_track, reinterpret_cast<jlong>(track.get()));
  return 0;
}

void ReleaseTrack(JNIEnv* env, jobject thiz) {
  JniTrack* jt;
  {
    std::lock_guard<std::mutex> lock(gLock);
    jt = reinterpret_cast<JniTrack*>(env->GetLongField(thiz, gFields.jni_data));
    env->SetLongField(thiz, gFields.jni_data, 0);
    env->SetLongField(thiz, gFields.native_track, 0);
  }
  if (jt == nullptr) return;
  jt->track->Release();
  delete jt;
}

void JNICALL AudioTrack_release(JNIEnv* env, jobject thiz) { ReleaseTrack(env, thiz); }
void JNICALL AudioTrack_finalize(JNIEnv* env, jobject thiz) { ReleaseTrack(env, thiz); }

void JNICALL AudioTrack_start(JNIEnv* env, jobject thiz) {
  if (auto t = GetTrackOrThrow(env, thiz, "start()")) t->Start();
}
void JNICALL AudioTrack_stop(JNIEnv* env, jobject thiz) {
  if (auto t = GetTrackOrThrow(env, thiz, "stop()")) t->Stop();
}
void JNICALL AudioTrack_pause(JNIEnv* env, jobject thiz) {
  if (auto t = GetTrackOrThrow(env, thiz, "pause()")) t->Pause();
}
void JNICALL AudioTrack_flush(JNIEnv* env, jobject thiz) {
  if (auto t = GetTrackOrThrow(env, thiz, "flush()")) t->Flush();
}

jint WriteBytes(JNIEnv* env, jobject thiz, const void* data, size_t bytes, bool blocking) {
  auto t = GetTrack(env, thiz);
  if (t == nullptr) {
    jniThrow(env, "java/lang/IllegalStateException", "Unable to retrieve AudioTrack pointer for write()");
    return kInvalidOperation;
  }
  ssize_t n = t->Write(data, bytes, blocking);
  return static_cast<jint>(n);
}

jint JNICALL AudioTrack_writeByte(JNIEnv* env, jobject thiz, jbyteArray data, jint offset, jint size,
                                  jint /*format*/, jboolean blocking) {
  if (data == nullptr || size < 0 || offset < 0) return kBadValue;
  std::vector<uint8_t> buf(static_cast<size_t>(size));
  env->GetByteArrayRegion(data, offset, size, reinterpret_cast<jbyte*>(buf.data()));
  if (env->ExceptionCheck()) return kBadValue;
  return WriteBytes(env, thiz, buf.data(), buf.size(), blocking);
}

jint JNICALL AudioTrack_writeShort(JNIEnv* env, jobject thiz, jshortArray data, jint offset, jint size,
                                   jint /*format*/, jboolean blocking) {
  if (data == nullptr || size < 0 || offset < 0) return kBadValue;
  std::vector<jshort> buf(static_cast<size_t>(size));
  env->GetShortArrayRegion(data, offset, size, buf.data());
  if (env->ExceptionCheck()) return kBadValue;
  jint n = WriteBytes(env, thiz, buf.data(), buf.size() * 2, blocking);
  return n > 0 ? n / 2 : n;
}

jint JNICALL AudioTrack_writeFloat(JNIEnv* env, jobject thiz, jfloatArray data, jint offset, jint size,
                                   jint /*format*/, jboolean blocking) {
  if (data == nullptr || size < 0 || offset < 0) return kBadValue;
  std::vector<jfloat> buf(static_cast<size_t>(size));
  env->GetFloatArrayRegion(data, offset, size, buf.data());
  if (env->ExceptionCheck()) return kBadValue;
  jint n = WriteBytes(env, thiz, buf.data(), buf.size() * 4, blocking);
  return n > 0 ? n / 4 : n;
}

jint JNICALL AudioTrack_writeNativeBytes(JNIEnv* env, jobject thiz, jobject buffer, jint position,
                                         jint size, jint /*format*/, jboolean blocking) {
  auto* base = static_cast<uint8_t*>(env->GetDirectBufferAddress(buffer));
  if (base == nullptr || size < 0 || position < 0) return kBadValue;
  return WriteBytes(env, thiz, base + position, static_cast<size_t>(size), blocking);
}

jint JNICALL AudioTrack_reloadStatic(JNIEnv* env, jobject thiz) {
  auto t = GetTrack(env, thiz);
  return t != nullptr ? t->Reload() : kInvalidOperation;
}

jint JNICALL AudioTrack_getBufferSizeFrames(JNIEnv* env, jobject thiz) {
  auto t = GetTrackOrThrow(env, thiz, "getBufferSizeInFrames()");
  return t != nullptr ? static_cast<jint>(t->buffer_size_frames()) : kError;
}

jint JNICALL AudioTrack_setBufferSizeFrames(JNIEnv* env, jobject thiz, jint frames) {
  auto t = GetTrackOrThrow(env, thiz, "setBufferSizeInFrames()");
  return t != nullptr ? t->SetBufferSizeFrames(static_cast<uint32_t>(std::max(frames, 0))) : kError;
}

jint JNICALL AudioTrack_getBufferCapacityFrames(JNIEnv* env, jobject thiz) {
  auto t = GetTrackOrThrow(env, thiz, "getBufferCapacityInFrames()");
  return t != nullptr ? static_cast<jint>(t->buffer_capacity_frames()) : kError;
}

void JNICALL AudioTrack_setVolume(JNIEnv* env, jobject thiz, jfloat left, jfloat right) {
  if (auto t = GetTrackOrThrow(env, thiz, "setVolume()")) t->SetVolume(left, right);
}

jint JNICALL AudioTrack_setPlaybackRate(JNIEnv* env, jobject thiz, jint rate) {
  auto t = GetTrack(env, thiz);
  return t != nullptr ? t->SetSampleRate(static_cast<uint32_t>(std::max(rate, 0))) : kInvalidOperation;
}

jint JNICALL AudioTrack_getPlaybackRate(JNIEnv* env, jobject thiz) {
  auto t = GetTrack(env, thiz);
  return t != nullptr ? static_cast<jint>(t->sample_rate()) : kInvalidOperation;
}

void JNICALL AudioTrack_setPlaybackParams(JNIEnv* env, jobject thiz, jobject params) {
  auto t = GetTrackOrThrow(env, thiz, "setPlaybackParams()");
  if (t == nullptr || params == nullptr) return;
  jclass cls = env->GetObjectClass(params);
  jfieldID set = env->GetFieldID(cls, "mSet", "I");
  jfieldID speed = env->GetFieldID(cls, "mSpeed", "F");
  if (set == nullptr || speed == nullptr) {
    env->ExceptionClear();
    return;
  }
  if ((env->GetIntField(params, set) & 1 /* SET_SPEED */) != 0) {
    t->SetSpeed(env->GetFloatField(params, speed));
  }
}

jobject JNICALL AudioTrack_getPlaybackParams(JNIEnv* env, jobject thiz) {
  auto t = GetTrackOrThrow(env, thiz, "getPlaybackParams()");
  if (t == nullptr) return nullptr;
  jclass cls = env->FindClass("android/media/PlaybackParams");
  jobject p = env->NewObject(cls, env->GetMethodID(cls, "<init>", "()V"));
  jmethodID setSpeed = env->GetMethodID(cls, "setSpeed", "(F)Landroid/media/PlaybackParams;");
  jmethodID setPitch = env->GetMethodID(cls, "setPitch", "(F)Landroid/media/PlaybackParams;");
  env->DeleteLocalRef(env->CallObjectMethod(p, setSpeed, t->speed()));
  env->DeleteLocalRef(env->CallObjectMethod(p, setPitch, t->speed()));
  return p;
}

jint JNICALL AudioTrack_setMarkerPos(JNIEnv* env, jobject thiz, jint pos) {
  auto t = GetTrack(env, thiz);
  return t != nullptr ? t->SetMarker(static_cast<uint32_t>(pos)) : kInvalidOperation;
}
jint JNICALL AudioTrack_getMarkerPos(JNIEnv* env, jobject thiz) {
  auto t = GetTrack(env, thiz);
  return t != nullptr ? static_cast<jint>(t->marker()) : kInvalidOperation;
}
jint JNICALL AudioTrack_setPosUpdatePeriod(JNIEnv* env, jobject thiz, jint period) {
  auto t = GetTrack(env, thiz);
  return t != nullptr ? t->SetUpdatePeriod(static_cast<uint32_t>(std::max(period, 0)))
                      : kInvalidOperation;
}
jint JNICALL AudioTrack_getPosUpdatePeriod(JNIEnv* env, jobject thiz) {
  auto t = GetTrack(env, thiz);
  return t != nullptr ? static_cast<jint>(t->update_period()) : kInvalidOperation;
}
jint JNICALL AudioTrack_setPosition(JNIEnv* env, jobject thiz, jint pos) {
  auto t = GetTrack(env, thiz);
  return t != nullptr ? t->SetPosition(static_cast<uint32_t>(pos)) : kInvalidOperation;
}
jint JNICALL AudioTrack_getPosition(JNIEnv* env, jobject thiz) {
  auto t = GetTrack(env, thiz);
  return t != nullptr ? static_cast<jint>(t->Position()) : kInvalidOperation;
}
jint JNICALL AudioTrack_getLatency(JNIEnv* env, jobject thiz) {
  auto t = GetTrack(env, thiz);
  return t != nullptr ? static_cast<jint>(t->LatencyMs()) : kInvalidOperation;
}
jint JNICALL AudioTrack_getUnderrunCount(JNIEnv* env, jobject thiz) {
  auto t = GetTrack(env, thiz);
  return t != nullptr ? static_cast<jint>(t->underrun_frames()) : kInvalidOperation;
}
jint JNICALL AudioTrack_getFlags(JNIEnv*, jobject) { return 0; }

jint JNICALL AudioTrack_getTimestamp(JNIEnv* env, jobject thiz, jlongArray out) {
  auto t = GetTrack(env, thiz);
  if (t == nullptr || out == nullptr) return kInvalidOperation;
  int64_t frames, ns;
  if (!t->Timestamp(&frames, &ns)) return kWouldBlock;
  jlong v[2] = {frames, ns};
  env->SetLongArrayRegion(out, 0, 2, v);
  return kOk;
}

jobject JNICALL AudioTrack_getMetrics(JNIEnv* env, jobject) {
  jclass cls = env->FindClass("android/os/PersistableBundle");
  return env->NewObject(cls, env->GetMethodID(cls, "<init>", "()V"));
}

jint JNICALL AudioTrack_setLoop(JNIEnv* env, jobject thiz, jint start, jint end, jint count) {
  auto t = GetTrack(env, thiz);
  return t != nullptr ? t->SetLoop(static_cast<uint32_t>(start), static_cast<uint32_t>(end), count)
                      : kInvalidOperation;
}

jint JNICALL AudioTrack_getOutputSampleRate(JNIEnv*, jclass, jint) {
  return static_cast<jint>(GetOutputInfo().sample_rate);
}

jint JNICALL AudioTrack_getMinBuffSize(JNIEnv*, jclass, jint rate, jint channels, jint format) {
  bool ok;
  PcmFormat f = ToPcm(format, &ok);
  if (!ok || rate <= 0 || channels <= 0) return kBadValue;
  // Two mixer periods at the output rate, in the track's frames, with at
  // least 20 ms: about what AudioFlinger asks of a normal track.
  OutputInfo info = GetOutputInfo();
  int64_t frames = std::max<int64_t>(
      static_cast<int64_t>(2) * info.period_frames * rate / std::max<uint32_t>(info.sample_rate, 1),
      rate / 50);
  frames = std::max<int64_t>(frames, 256);
  return static_cast<jint>(frames * channels * static_cast<int64_t>(BytesPerSample(f)));
}

jint JNICALL AudioTrack_attachAuxEffect(JNIEnv*, jobject, jint) { return kOk; }
jint JNICALL AudioTrack_setAuxEffectSendLevel(JNIEnv*, jobject, jfloat) { return kOk; }
jboolean JNICALL AudioTrack_setOutputDevice(JNIEnv*, jobject, jint) { return JNI_TRUE; }

jintArray JNICALL AudioTrack_getRoutedDeviceIds(JNIEnv* env, jobject) {
  jintArray a = env->NewIntArray(1);
  jint id = kOutputDeviceId;
  env->SetIntArrayRegion(a, 0, 1, &id);
  return a;
}

jint JNICALL AudioTrack_applyVolumeShaper(JNIEnv*, jobject, jobject, jobject) {
  // VolumeShapers are not applied (no fades); the id lets the caller go on.
  static std::atomic<int> next{1};
  return next++;
}

jint JNICALL AudioTrack_getPortId(JNIEnv*, jobject) { return kOutputDeviceId; }

const JNINativeMethod kMethods[] = {
    {"native_start", "()V", GESHER_NATIVE(AudioTrack_start)},
    {"native_stop", "()V", GESHER_NATIVE(AudioTrack_stop)},
    {"native_pause", "()V", GESHER_NATIVE(AudioTrack_pause)},
    {"native_flush", "()V", GESHER_NATIVE(AudioTrack_flush)},
    {"native_setup",
     "(Ljava/lang/Object;Ljava/lang/Object;[IIIIII[ILandroid/os/Parcel;JZILjava/lang/Object;"
     "Ljava/lang/String;)I",
     GESHER_NATIVE(AudioTrack_setup)},
    {"native_finalize", "()V", GESHER_NATIVE(AudioTrack_finalize)},
    {"native_release", "()V", GESHER_NATIVE(AudioTrack_release)},
    {"native_write_byte", "([BIIIZ)I", GESHER_NATIVE(AudioTrack_writeByte)},
    {"native_write_native_bytes", "(Ljava/nio/ByteBuffer;IIIZ)I",
     GESHER_NATIVE(AudioTrack_writeNativeBytes)},
    {"native_write_short", "([SIIIZ)I", GESHER_NATIVE(AudioTrack_writeShort)},
    {"native_write_float", "([FIIIZ)I", GESHER_NATIVE(AudioTrack_writeFloat)},
    {"native_reload_static", "()I", GESHER_NATIVE(AudioTrack_reloadStatic)},
    {"native_get_buffer_size_frames", "()I", GESHER_NATIVE(AudioTrack_getBufferSizeFrames)},
    {"native_set_buffer_size_frames", "(I)I", GESHER_NATIVE(AudioTrack_setBufferSizeFrames)},
    {"native_get_buffer_capacity_frames", "()I", GESHER_NATIVE(AudioTrack_getBufferCapacityFrames)},
    {"native_setVolume", "(FF)V", GESHER_NATIVE(AudioTrack_setVolume)},
    {"native_set_playback_rate", "(I)I", GESHER_NATIVE(AudioTrack_setPlaybackRate)},
    {"native_get_playback_rate", "()I", GESHER_NATIVE(AudioTrack_getPlaybackRate)},
    {"native_set_playback_params", "(Landroid/media/PlaybackParams;)V",
     GESHER_NATIVE(AudioTrack_setPlaybackParams)},
    {"native_get_playback_params", "()Landroid/media/PlaybackParams;",
     GESHER_NATIVE(AudioTrack_getPlaybackParams)},
    {"native_set_marker_pos", "(I)I", GESHER_NATIVE(AudioTrack_setMarkerPos)},
    {"native_get_marker_pos", "()I", GESHER_NATIVE(AudioTrack_getMarkerPos)},
    {"native_set_pos_update_period", "(I)I", GESHER_NATIVE(AudioTrack_setPosUpdatePeriod)},
    {"native_get_pos_update_period", "()I", GESHER_NATIVE(AudioTrack_getPosUpdatePeriod)},
    {"native_set_position", "(I)I", GESHER_NATIVE(AudioTrack_setPosition)},
    {"native_get_position", "()I", GESHER_NATIVE(AudioTrack_getPosition)},
    {"native_get_latency", "()I", GESHER_NATIVE(AudioTrack_getLatency)},
    {"native_get_underrun_count", "()I", GESHER_NATIVE(AudioTrack_getUnderrunCount)},
    {"native_get_flags", "()I", GESHER_NATIVE(AudioTrack_getFlags)},
    {"native_get_timestamp", "([J)I", GESHER_NATIVE(AudioTrack_getTimestamp)},
    {"native_getMetrics", "()Landroid/os/PersistableBundle;", GESHER_NATIVE(AudioTrack_getMetrics)},
    {"native_set_loop", "(III)I", GESHER_NATIVE(AudioTrack_setLoop)},
    {"native_get_output_sample_rate", "(I)I", GESHER_NATIVE(AudioTrack_getOutputSampleRate)},
    {"native_get_min_buff_size", "(III)I", GESHER_NATIVE(AudioTrack_getMinBuffSize)},
    {"native_setAuxEffectSendLevel", "(F)I", GESHER_NATIVE(AudioTrack_setAuxEffectSendLevel)},
    {"native_attachAuxEffect", "(I)I", GESHER_NATIVE(AudioTrack_attachAuxEffect)},
    {"native_setOutputDevice", "(I)Z", GESHER_NATIVE(AudioTrack_setOutputDevice)},
    {"native_getRoutedDeviceIds", "()[I", GESHER_NATIVE(AudioTrack_getRoutedDeviceIds)},
    {"native_applyVolumeShaper",
     "(Landroid/media/VolumeShaper$Configuration;Landroid/media/VolumeShaper$Operation;)I",
     GESHER_NATIVE(AudioTrack_applyVolumeShaper)},
    {"native_getPortId", "()I", GESHER_NATIVE(AudioTrack_getPortId)},
};

}  // namespace

int RegisterAudioTrack(JNIEnv* env) {
  const char* name = "android/media/AudioTrack";
  if (!RegisterStandins(env, name)) return 0;
  jclass cls = env->FindClass(name);
  gFields.clazz = static_cast<jclass>(env->NewGlobalRef(cls));
  gFields.native_track = env->GetFieldID(cls, "mNativeTrackInJavaObj", "J");
  gFields.jni_data = env->GetFieldID(cls, "mJniData", "J");
  gFields.stream_type = env->GetFieldID(cls, "mStreamType", "I");
  gFields.post_event = env->GetStaticMethodID(
      cls, "postEventFromNative", "(Ljava/lang/Object;IIILjava/lang/Object;)V");
  env->DeleteLocalRef(cls);
  if (gFields.jni_data == nullptr || gFields.post_event == nullptr) {
    env->ExceptionClear();
    MEDIA_LOGE("AudioTrack's fields are not as expected; its natives stay stand-ins");
    return 0;
  }
  RegisterMethods(env, name, kMethods);
  return 0;
}

}  // namespace gesher::media
