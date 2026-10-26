// Gesher: android.media.AudioRecord's natives over the capture engine
// (compat/media/audio/capture.h; docs/media.md "Recording", ADR 0059).
//
// In place of frameworks/base core/jni/android_media_AudioRecord.cpp, which
// drives libaudioclient's AudioRecord against AudioFlinger: each Java
// AudioRecord owns a CaptureStream of this process's capture engine. As
// AudioFlinger does, the setup refuses an app whose manifest lacks
// RECORD_AUDIO (the record stays uninitialized; startRecording throws).
// Markers and periodic positions come back through
// AudioRecord.postEventFromNative, from the capture thread.

#include <jni.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>

#include "../audio/capture.h"
#include "../audio/engine.h"
#include "../audio/log.h"
#include "../audio/shared_state.h"
#include "jni_util.h"
#include "media_jni.h"

namespace gesher::media {
namespace {

constexpr int kEncodingPcm16 = 2;
constexpr int kEncodingPcm8 = 3;
constexpr int kEncodingPcmFloat = 4;
constexpr int kEncodingPcm24Packed = 21;
constexpr int kEncodingPcm32 = 22;
constexpr int kEncodingDefault = 1;
// AudioRecord's AUDIORECORD_ERROR_SETUP_* codes and native events.
constexpr int kSetupZeroFrameCount = -16;
constexpr int kSetupInvalidChannelMask = -17;
constexpr int kSetupInvalidFormat = -18;
constexpr int kSetupNativeInitFailed = -20;
constexpr int kEventMarker = 2;
constexpr int kEventNewPos = 3;
// AudioSystem's Java status codes.
constexpr int kJavaSuccess = 0;
constexpr int kJavaError = -1;
constexpr int kJavaBadValue = -2;
constexpr int kJavaInvalidOperation = -3;

struct {
  jfieldID native_handle;  // long mNativeAudioRecordHandle
  jfieldID jni_data;       // long mNativeJNIDataHandle
  jclass clazz;
  jmethodID post_event;    // static postEventFromNative(Object, int, int, int, Object)
} gFields;

std::mutex gLock;

struct JniRecord {
  std::shared_ptr<CaptureStream> stream;
  std::shared_ptr<GlobalRef> weak_this;
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

std::shared_ptr<CaptureStream> GetStream(JNIEnv* env, jobject thiz) {
  std::lock_guard<std::mutex> lock(gLock);
  auto* jr = reinterpret_cast<JniRecord*>(env->GetLongField(thiz, gFields.jni_data));
  return jr != nullptr ? jr->stream : nullptr;
}

void PostToJava(const std::shared_ptr<GlobalRef>& weak, int what, int arg1) {
  JNIEnv* env = AttachedEnv("gesher-record-cb");
  if (env == nullptr) return;
  env->CallStaticVoidMethod(gFields.clazz, gFields.post_event, weak->get(), what, arg1, 0, nullptr);
  CheckException(env, "AudioRecord.postEventFromNative");
}

jint JNICALL AR_setup(JNIEnv* env, jobject thiz, jobject weak_this, jobject attributes,
                      jintArray jSampleRate, jint channelMask, jint channelIndexMask, jint audioFormat,
                      jint buffSizeInBytes, jintArray jSession, jobject /*attributionSource*/,
                      jlong nativeRecordInJavaObj, jint /*maxSharedAudioHistoryMs*/,
                      jint /*halInputFlags*/) {
  if (nativeRecordInJavaObj != 0) {
    MEDIA_LOGW("AudioRecord: native AudioRecords are not supported");
    return kSetupNativeInitFailed;
  }
  if (jSampleRate == nullptr || jSession == nullptr || attributes == nullptr) return kJavaError;
  if (!MayRecordAudio(env)) {
    MEDIA_LOGE("AudioRecord: the app does not have android.permission.RECORD_AUDIO");
    return kSetupNativeInitFailed;
  }
  EnsureAppName(env);
  int channels = channelIndexMask != 0 ? __builtin_popcount(static_cast<unsigned>(channelIndexMask))
                                       : __builtin_popcount(static_cast<unsigned>(channelMask));
  if (channels <= 0 || channels > 8) {
    MEDIA_LOGE("AudioRecord: channel mask 0x%x, index mask 0x%x", channelMask, channelIndexMask);
    return kSetupInvalidChannelMask;
  }
  bool ok;
  PcmFormat format = ToPcm(audioFormat, &ok);
  if (!ok) {
    MEDIA_LOGE("AudioRecord: encoding %d is not supported", audioFormat);
    return kSetupInvalidFormat;
  }
  if (buffSizeInBytes <= 0) return kSetupZeroFrameCount;
  jint rate = 0;
  env->GetIntArrayRegion(jSampleRate, 0, 1, &rate);
  if (rate == 0) {
    rate = static_cast<jint>(GetInputInfo().sample_rate);
    env->SetIntArrayRegion(jSampleRate, 0, 1, &rate);
  }
  jint session = 0;
  env->GetIntArrayRegion(jSession, 0, 1, &session);
  if (session == 0) {
    session = NewSessionId();
    env->SetIntArrayRegion(jSession, 0, 1, &session);
  }
  CaptureParams p;
  p.sample_rate = static_cast<uint32_t>(rate);
  p.channels = static_cast<uint32_t>(channels);
  p.format = format;
  const size_t frame = BytesPerSample(format) * static_cast<size_t>(channels);
  p.capacity_frames = std::max(static_cast<size_t>(buffSizeInBytes) / frame,
                               MinCaptureFrames(static_cast<uint32_t>(rate)));
  p.name = "AudioRecord";
  std::string error;
  auto stream = OpenCapture(p, &error);
  if (stream == nullptr) {
    MEDIA_LOGE("AudioRecord: %s", error.c_str());
    return kSetupNativeInitFailed;
  }
  auto* jr = new JniRecord();
  jr->stream = stream;
  jr->weak_this = std::make_shared<GlobalRef>(env, weak_this);
  std::weak_ptr<GlobalRef> weak = jr->weak_this;
  CaptureEvents events;
  events.marker = [weak](uint32_t pos) {
    if (auto w = weak.lock()) PostToJava(w, kEventMarker, static_cast<int>(pos));
  };
  events.new_position = [weak](uint32_t pos) {
    if (auto w = weak.lock()) PostToJava(w, kEventNewPos, static_cast<int>(pos));
  };
  stream->SetEvents(std::move(events));
  {
    std::lock_guard<std::mutex> lock(gLock);
    env->SetLongField(thiz, gFields.jni_data, reinterpret_cast<jlong>(jr));
    env->SetLongField(thiz, gFields.native_handle, reinterpret_cast<jlong>(jr));
  }
  MEDIA_LOGI("AudioRecord: %d Hz, %d channels, encoding %d, %zu frames buffered (%s)", rate, channels,
             audioFormat, p.capacity_frames, GetInputInfo().description.c_str());
  return kJavaSuccess;
}

void Release(JNIEnv* env, jobject thiz) {
  JniRecord* jr;
  {
    std::lock_guard<std::mutex> lock(gLock);
    jr = reinterpret_cast<JniRecord*>(env->GetLongField(thiz, gFields.jni_data));
    env->SetLongField(thiz, gFields.jni_data, 0);
    env->SetLongField(thiz, gFields.native_handle, 0);
  }
  if (jr == nullptr) return;
  jr->stream->Release();
  delete jr;
}

void JNICALL AR_release(JNIEnv* env, jobject thiz) { Release(env, thiz); }
void JNICALL AR_finalize(JNIEnv* env, jobject thiz) { Release(env, thiz); }

jint JNICALL AR_start(JNIEnv* env, jobject thiz, jint, jint) {
  auto s = GetStream(env, thiz);
  if (s == nullptr) {
    jniThrow(env, "java/lang/IllegalStateException", nullptr);
    return kJavaError;
  }
  return s->Start() == kOk ? kJavaSuccess : kJavaInvalidOperation;
}

void JNICALL AR_stop(JNIEnv* env, jobject thiz) {
  auto s = GetStream(env, thiz);
  if (s == nullptr) {
    jniThrow(env, "java/lang/IllegalStateException", nullptr);
    return;
  }
  s->Stop();
}

// Reads `bytes` into `dst`; the Java answer (bytes read, or an error).
jint ReadInto(JNIEnv* env, jobject thiz, void* dst, size_t bytes, bool blocking) {
  auto s = GetStream(env, thiz);
  if (s == nullptr) return kJavaInvalidOperation;
  ssize_t n = s->Read(dst, bytes, blocking);
  return n < 0 ? kJavaError : static_cast<jint>(n);
}

jint JNICALL AR_readBytes(JNIEnv* env, jobject thiz, jbyteArray data, jint offset, jint size,
                          jboolean blocking) {
  if (data == nullptr) return kJavaBadValue;
  if (offset < 0 || size < 0 || offset + size > env->GetArrayLength(data)) return kJavaBadValue;
  jbyte* p = env->GetByteArrayElements(data, nullptr);
  if (p == nullptr) return kJavaBadValue;
  jint n = ReadInto(env, thiz, p + offset, static_cast<size_t>(size), blocking);
  env->ReleaseByteArrayElements(data, p, 0);
  return n;
}

jint JNICALL AR_readShorts(JNIEnv* env, jobject thiz, jshortArray data, jint offset, jint size,
                           jboolean blocking) {
  if (data == nullptr) return kJavaBadValue;
  if (offset < 0 || size < 0 || offset + size > env->GetArrayLength(data)) return kJavaBadValue;
  jshort* p = env->GetShortArrayElements(data, nullptr);
  if (p == nullptr) return kJavaBadValue;
  jint n = ReadInto(env, thiz, p + offset, static_cast<size_t>(size) * 2, blocking);
  env->ReleaseShortArrayElements(data, p, 0);
  return n < 0 ? n : n / 2;
}

jint JNICALL AR_readFloats(JNIEnv* env, jobject thiz, jfloatArray data, jint offset, jint size,
                           jboolean blocking) {
  if (data == nullptr) return kJavaBadValue;
  if (offset < 0 || size < 0 || offset + size > env->GetArrayLength(data)) return kJavaBadValue;
  jfloat* p = env->GetFloatArrayElements(data, nullptr);
  if (p == nullptr) return kJavaBadValue;
  jint n = ReadInto(env, thiz, p + offset, static_cast<size_t>(size) * 4, blocking);
  env->ReleaseFloatArrayElements(data, p, 0);
  return n < 0 ? n : n / 4;
}

jint JNICALL AR_readDirect(JNIEnv* env, jobject thiz, jobject buffer, jint size, jboolean blocking) {
  void* p = env->GetDirectBufferAddress(buffer);
  jlong cap = env->GetDirectBufferCapacity(buffer);
  if (p == nullptr || cap < size) return kJavaBadValue;
  return ReadInto(env, thiz, p, static_cast<size_t>(size), blocking);
}

jint JNICALL AR_bufferSizeInFrames(JNIEnv* env, jobject thiz) {
  auto s = GetStream(env, thiz);
  if (s == nullptr) {
    jniThrow(env, "java/lang/IllegalStateException", nullptr);
    return -1;
  }
  return static_cast<jint>(s->capacity_frames());
}

jint JNICALL AR_setMarker(JNIEnv* env, jobject thiz, jint marker) {
  auto s = GetStream(env, thiz);
  return s != nullptr ? s->SetMarker(static_cast<uint32_t>(marker)) : kJavaError;
}
jint JNICALL AR_getMarker(JNIEnv* env, jobject thiz) {
  auto s = GetStream(env, thiz);
  return s != nullptr ? static_cast<jint>(s->marker()) : kJavaError;
}
jint JNICALL AR_setPeriod(JNIEnv* env, jobject thiz, jint period) {
  auto s = GetStream(env, thiz);
  return s != nullptr ? s->SetUpdatePeriod(static_cast<uint32_t>(period)) : kJavaError;
}
jint JNICALL AR_getPeriod(JNIEnv* env, jobject thiz) {
  auto s = GetStream(env, thiz);
  return s != nullptr ? static_cast<jint>(s->update_period()) : kJavaError;
}

jint JNICALL AR_minBufferSize(JNIEnv*, jclass, jint rate, jint channels, jint encoding) {
  bool ok;
  PcmFormat f = ToPcm(encoding, &ok);
  if (!ok || rate <= 0 || channels <= 0) return kJavaBadValue;
  return static_cast<jint>(MinCaptureFrames(static_cast<uint32_t>(rate)) * BytesPerSample(f) *
                           static_cast<size_t>(channels));
}

jint JNICALL AR_timestamp(JNIEnv* env, jobject thiz, jobject out, jint /*timebase*/) {
  auto s = GetStream(env, thiz);
  if (s == nullptr || out == nullptr) return kJavaError;
  int64_t frames, ns;
  if (!s->Timestamp(&frames, &ns)) return kJavaInvalidOperation;
  jclass cls = env->GetObjectClass(out);
  env->SetLongField(out, env->GetFieldID(cls, "framePosition", "J"), frames);
  env->SetLongField(out, env->GetFieldID(cls, "nanoTime", "J"), ns);
  return kJavaSuccess;
}

// The input's AudioDeviceInfo id (AudioSystem's input port).
jintArray JNICALL AR_routedDevices(JNIEnv* env, jobject) {
  const bool device = GetInputInfo().device;
  jintArray a = env->NewIntArray(device ? 1 : 0);
  if (device) {
    jint id = kInputDeviceId;
    env->SetIntArrayRegion(a, 0, 1, &id);
  }
  return a;
}

jboolean JNICALL AR_setInputDevice(JNIEnv*, jobject, jint) { return JNI_TRUE; }

const JNINativeMethod kMethods[] = {
    {"native_start", "(II)I", GESHER_NATIVE(AR_start)},
    {"native_stop", "()V", GESHER_NATIVE(AR_stop)},
    {"native_setup", "(Ljava/lang/Object;Ljava/lang/Object;[IIIII[ILandroid/os/Parcel;JII)I",
     GESHER_NATIVE(AR_setup)},
    {"native_finalize", "()V", GESHER_NATIVE(AR_finalize)},
    {"native_release", "()V", GESHER_NATIVE(AR_release)},
    {"native_read_in_byte_array", "([BIIZ)I", GESHER_NATIVE(AR_readBytes)},
    {"native_read_in_short_array", "([SIIZ)I", GESHER_NATIVE(AR_readShorts)},
    {"native_read_in_float_array", "([FIIZ)I", GESHER_NATIVE(AR_readFloats)},
    {"native_read_in_direct_buffer", "(Ljava/lang/Object;IZ)I", GESHER_NATIVE(AR_readDirect)},
    {"native_get_buffer_size_in_frames", "()I", GESHER_NATIVE(AR_bufferSizeInFrames)},
    {"native_set_marker_pos", "(I)I", GESHER_NATIVE(AR_setMarker)},
    {"native_get_marker_pos", "()I", GESHER_NATIVE(AR_getMarker)},
    {"native_set_pos_update_period", "(I)I", GESHER_NATIVE(AR_setPeriod)},
    {"native_get_pos_update_period", "()I", GESHER_NATIVE(AR_getPeriod)},
    {"native_get_min_buff_size", "(III)I", GESHER_NATIVE(AR_minBufferSize)},
    {"native_setInputDevice", "(I)Z", GESHER_NATIVE(AR_setInputDevice)},
    {"native_getRoutedDeviceIds", "()[I", GESHER_NATIVE(AR_routedDevices)},
    {"native_get_timestamp", "(Landroid/media/AudioTimestamp;I)I", GESHER_NATIVE(AR_timestamp)},
};

}  // namespace

bool MayRecordAudio(JNIEnv* env) {
  if (env == nullptr) env = AttachedEnv("gesher-record");
  if (env == nullptr) return false;
  jclass cls = env->FindClass("com/gesher/server/AudioService");
  if (cls == nullptr) {
    // Not on Gesher's boot class path (a bare runtime): nothing to ask.
    env->ExceptionClear();
    return true;
  }
  jmethodID m = env->GetStaticMethodID(cls, "recordAudioAllowed", "()Z");
  if (m == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return true;
  }
  jboolean ok = env->CallStaticBooleanMethod(cls, m);
  if (CheckException(env, "AudioService.recordAudioAllowed")) ok = JNI_FALSE;
  env->DeleteLocalRef(cls);
  return ok == JNI_TRUE;
}

int RegisterAudioRecord(JNIEnv* env) {
  if (!RegisterStandins(env, "android/media/AudioRecord")) return 0;
  jclass cls = env->FindClass("android/media/AudioRecord");
  if (cls == nullptr) {
    env->ExceptionClear();
    return 0;
  }
  gFields.clazz = static_cast<jclass>(env->NewGlobalRef(cls));
  gFields.native_handle = env->GetFieldID(cls, "mNativeAudioRecordHandle", "J");
  gFields.jni_data = env->GetFieldID(cls, "mNativeJNIDataHandle", "J");
  gFields.post_event = env->GetStaticMethodID(cls, "postEventFromNative",
                                              "(Ljava/lang/Object;IIILjava/lang/Object;)V");
  if (gFields.native_handle == nullptr || gFields.jni_data == nullptr || gFields.post_event == nullptr) {
    CheckException(env, "AudioRecord's fields");
    return 0;
  }
  RegisterMethods(env, "android/media/AudioRecord", kMethods);
  return 0;
}

}  // namespace gesher::media
