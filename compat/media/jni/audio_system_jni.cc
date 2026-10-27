// Gesher: android.media.AudioSystem's natives, and the audio policy's
// (AudioProductStrategy), over the engine and the device's shared audio state
// (docs/media.md).
//
// On a device these reach AudioFlinger and the audio policy service. Here
// AudioSystem answers from this process's engine (the output's rate, its
// device) and from the shared state (shared_state.h): the system server's
// AudioService sets the stream volumes through initStreamVolume and
// setStreamVolumeIndex as it would AudioFlinger's, and every app process's
// engine plays at them. Everything else answers as a device with one output
// and no policy engine would (stand-ins: zero, false, null).

#include <jni.h>

#include <atomic>
#include <cmath>
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

// --- AudioSystem ---------------------------------------------------------------------------

jint JNICALL AS_getMaxChannelCount(JNIEnv*, jclass) { return 24; }    // FCC_24
jint JNICALL AS_getMaxSampleRate(JNIEnv*, jclass) { return 192000; }  // SAMPLE_RATE_HZ_MAX
jint JNICALL AS_getMinSampleRate(JNIEnv*, jclass) { return 4000; }    // SAMPLE_RATE_HZ_MIN

jboolean JNICALL AS_isStreamActive(JNIEnv*, jclass, jint stream, jint inPastMs) {
  return IsStreamActive(stream, inPastMs) ? JNI_TRUE : JNI_FALSE;
}

jint JNICALL AS_newAudioSessionId(JNIEnv*, jclass) { return NewSessionId(); }

jint JNICALL AS_newAudioPlayerId(JNIEnv*, jclass) {
  // AUDIO_UNIQUE_ID_USE_PLAYER (7) in the low bits, as AudioFlinger's ids.
  return (NewSessionId() & ~7) | 7;
}

jint JNICALL AS_newAudioRecorderId(JNIEnv*, jclass) {
  return (NewSessionId() & ~7) | 6;  // AUDIO_UNIQUE_ID_USE_RECORD
}

jint JNICALL AS_setParameters(JNIEnv*, jclass, jstring) { return kOk; }

jstring JNICALL AS_getParameters(JNIEnv* env, jclass, jstring) { return env->NewStringUTF(""); }

jint JNICALL AS_initStreamVolume(JNIEnv*, jclass, jint stream, jint min, jint max) {
  InitStreamVolume(stream, min, max);
  return kOk;
}

jint JNICALL AS_setStreamVolumeIndex(JNIEnv*, jclass, jint stream, jint index, jboolean muted,
                                     jint /*device*/) {
  SetStreamVolume(stream, index, muted);
  return kOk;
}

jint JNICALL AS_getStreamVolumeIndex(JNIEnv*, jclass, jint stream, jint /*device*/) {
  return GetStreamVolume(stream);
}

jfloat JNICALL AS_getStreamVolumeDB(JNIEnv*, jclass, jint stream, jint index, jint) {
  int max = GetStreamMaxVolume(stream);
  if (index <= 0 || max <= 0) return -758.0f;  // AudioSystem's "silent" (VOLUME_MIN_DB)
  return static_cast<jfloat>(20.0 * std::log10(static_cast<double>(index) / max));
}

jint JNICALL AS_setMasterVolume(JNIEnv*, jclass, jfloat v) {
  SetMasterVolume(v);
  return kOk;
}
jfloat JNICALL AS_getMasterVolume(JNIEnv*, jclass) { return GetMasterVolume(); }
jint JNICALL AS_setMasterMute(JNIEnv*, jclass, jboolean mute) {
  SetMasterMute(mute);
  return kOk;
}
jboolean JNICALL AS_getMasterMute(JNIEnv*, jclass) { return GetMasterMute() ? JNI_TRUE : JNI_FALSE; }

jint JNICALL AS_getPrimaryOutputSamplingRate(JNIEnv*, jclass) {
  return static_cast<jint>(GetOutputInfo().sample_rate);
}
jint JNICALL AS_getPrimaryOutputFrameCount(JNIEnv*, jclass) {
  return static_cast<jint>(GetOutputInfo().period_frames);
}
jint JNICALL AS_getOutputLatency(JNIEnv*, jclass, jint) {
  OutputInfo info = GetOutputInfo();
  return static_cast<jint>(info.latency_frames * 1000 / std::max<uint32_t>(info.sample_rate, 1));
}

// The output as an AudioDevicePort: one profile per PCM encoding the engine
// takes, at the rates it resamples from.
jobject DevicePort(JNIEnv* env, bool input, int id, const std::string& device_name, int device_type) {
  jclass handleCls = env->FindClass("android/media/AudioHandle");
  jobject handle = env->NewObject(handleCls, env->GetMethodID(handleCls, "<init>", "(I)V"), id);
  static const jint kRates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000,
                                44100, 48000, 88200, 96000, 192000};
  jintArray rates = env->NewIntArray(sizeof(kRates) / sizeof(kRates[0]));
  env->SetIntArrayRegion(rates, 0, sizeof(kRates) / sizeof(kRates[0]), kRates);
  // CHANNEL_OUT_MONO and _STEREO; CHANNEL_IN_MONO and _STEREO.
  const jint masks[] = {input ? 16 : 4, 12};
  jintArray channelMasks = env->NewIntArray(2);
  env->SetIntArrayRegion(channelMasks, 0, 2, masks);
  jintArray indexMasks = env->NewIntArray(0);
  jclass profileCls = env->FindClass("android/media/AudioProfile");
  jmethodID profileInit = env->GetMethodID(profileCls, "<init>", "(I[I[I[II)V");
  jclass listCls = env->FindClass("java/util/ArrayList");
  jmethodID listInit = env->GetMethodID(listCls, "<init>", "()V");
  jmethodID listAdd = env->GetMethodID(listCls, "add", "(Ljava/lang/Object;)Z");
  jobject profiles = env->NewObject(listCls, listInit);
  for (jint encoding : {2 /* PCM_16BIT */, 3 /* PCM_8BIT */, 4 /* PCM_FLOAT */, 21, 22}) {
    jobject p = env->NewObject(profileCls, profileInit, encoding, rates, channelMasks, indexMasks, 0);
    env->CallBooleanMethod(profiles, listAdd, p);
    env->DeleteLocalRef(p);
  }
  jclass gainCls = env->FindClass("android/media/AudioGain");
  jobjectArray gains = env->NewObjectArray(0, gainCls, nullptr);
  jobject descriptors = env->NewObject(listCls, listInit);
  jintArray none = env->NewIntArray(0);
  jclass portCls = env->FindClass("android/media/AudioDevicePort");
  jmethodID portInit = env->GetMethodID(
      portCls, "<init>",
      "(Landroid/media/AudioHandle;Ljava/lang/String;Ljava/util/List;[Landroid/media/AudioGain;"
      "ILjava/lang/String;I[I[ILjava/util/List;)V");
  if (portInit == nullptr) {
    CheckException(env, "AudioDevicePort's constructor");
    return nullptr;
  }
  jstring name = env->NewStringUTF(device_name.empty() ? (input ? "Gesher audio input" : "Gesher audio output")
                                                       : device_name.c_str());
  jstring address = env->NewStringUTF("");
  jint layout = !input && device_type == 0x2 ? 12 /* CHANNEL_OUT_STEREO */ : 0 /* CHANNEL_INVALID */;
  jobject port = env->NewObject(portCls, portInit, handle, name, profiles, gains,
                                static_cast<jint>(device_type), address, layout, none, none,
                                descriptors);
  CheckException(env, "new AudioDevicePort");
  return port;
}

jint JNICALL AS_listAudioPorts(JNIEnv* env, jclass, jobject ports, jintArray generation) {
  OutputInfo info = GetOutputInfo();
  jobject port = DevicePort(env, false, kOutputDeviceId, info.device_name, info.device_type);
  jclass listCls = env->FindClass("java/util/ArrayList");
  jmethodID add = env->GetMethodID(listCls, "add", "(Ljava/lang/Object;)Z");
  if (port != nullptr && ports != nullptr) env->CallBooleanMethod(ports, add, port);
  // The input (media3, ADR 0059): a microphone, the loopback or GESHER_AUDIO_SOURCE's file;
  // none when the capture engine has nothing but silence.
  InputInfo input = GetInputInfo();
  if (input.device && ports != nullptr) {
    jobject in = DevicePort(env, true, kInputDeviceId, input.device_name, input.device_type);
    if (in != nullptr) env->CallBooleanMethod(ports, add, in);
  }
  if (generation != nullptr) {
    jint g = static_cast<jint>(info.generation);
    env->SetIntArrayRegion(generation, 0, 1, &g);
  }
  return kOk;
}

jint JNICALL AS_listAudioPatches(JNIEnv* env, jclass, jobject, jintArray generation) {
  // No patches; the generation matches the ports' (AudioManager waits for that).
  if (generation != nullptr) {
    jint g = static_cast<jint>(GetOutputInfo().generation);
    env->SetIntArrayRegion(generation, 0, 1, &g);
  }
  return kOk;
}

jint JNICALL AS_getDevicesForAttributes(JNIEnv* env, jclass, jobject, jobjectArray devices, jboolean) {
  if (devices == nullptr || env->GetArrayLength(devices) < 1) return kBadValue;
  jclass cls = env->FindClass("android/media/AudioDeviceAttributes");
  jmethodID init = env->GetMethodID(cls, "<init>", "(ILjava/lang/String;)V");
  jstring address = env->NewStringUTF("");
  jobject d = env->NewObject(cls, init, static_cast<jint>(GetOutputInfo().device_type), address);
  env->SetObjectArrayElement(devices, 0, d);
  return kOk;
}

jint JNICALL AS_getSupportedDeviceTypes(JNIEnv* env, jclass, jint flags, jobject intArray) {
  if (intArray == nullptr) return kOk;
  jclass cls = env->GetObjectClass(intArray);
  jmethodID add = env->GetMethodID(cls, "add", "(I)V");
  if (add == nullptr) {
    env->ExceptionClear();
    return kOk;
  }
  if (flags & 2 /* GET_DEVICES_OUTPUTS */) {
    env->CallVoidMethod(intArray, add, static_cast<jint>(GetOutputInfo().device_type));
  }
  if (flags & 1 /* GET_DEVICES_INPUTS */) {
    InputInfo input = GetInputInfo();
    if (input.device) env->CallVoidMethod(intArray, add, static_cast<jint>(input.device_type));
  }
  return kOk;
}

const JNINativeMethod kFrameworkCapabilities[] = {
    {"native_getMaxChannelCount", "()I", GESHER_NATIVE(AS_getMaxChannelCount)},
    {"native_getMaxSampleRate", "()I", GESHER_NATIVE(AS_getMaxSampleRate)},
    {"native_getMinSampleRate", "()I", GESHER_NATIVE(AS_getMinSampleRate)},
};

const JNINativeMethod kAudioSystemMethods[] = {
    {"isStreamActive", "(II)Z", GESHER_NATIVE(AS_isStreamActive)},
    {"newAudioSessionId", "()I", GESHER_NATIVE(AS_newAudioSessionId)},
    {"newAudioPlayerId", "()I", GESHER_NATIVE(AS_newAudioPlayerId)},
    {"newAudioRecorderId", "()I", GESHER_NATIVE(AS_newAudioRecorderId)},
    {"setParameters", "(Ljava/lang/String;)I", GESHER_NATIVE(AS_setParameters)},
    {"getParameters", "(Ljava/lang/String;)Ljava/lang/String;", GESHER_NATIVE(AS_getParameters)},
    {"initStreamVolume", "(III)I", GESHER_NATIVE(AS_initStreamVolume)},
    {"setStreamVolumeIndex", "(IIZI)I", GESHER_NATIVE(AS_setStreamVolumeIndex)},
    {"getStreamVolumeIndex", "(II)I", GESHER_NATIVE(AS_getStreamVolumeIndex)},
    {"getStreamVolumeDB", "(III)F", GESHER_NATIVE(AS_getStreamVolumeDB)},
    {"setMasterVolume", "(F)I", GESHER_NATIVE(AS_setMasterVolume)},
    {"getMasterVolume", "()F", GESHER_NATIVE(AS_getMasterVolume)},
    {"setMasterMute", "(Z)I", GESHER_NATIVE(AS_setMasterMute)},
    {"getMasterMute", "()Z", GESHER_NATIVE(AS_getMasterMute)},
    {"getPrimaryOutputSamplingRate", "()I", GESHER_NATIVE(AS_getPrimaryOutputSamplingRate)},
    {"getPrimaryOutputFrameCount", "()I", GESHER_NATIVE(AS_getPrimaryOutputFrameCount)},
    {"getOutputLatency", "(I)I", GESHER_NATIVE(AS_getOutputLatency)},
    {"listAudioPorts", "(Ljava/util/ArrayList;[I)I", GESHER_NATIVE(AS_listAudioPorts)},
    {"listAudioPatches", "(Ljava/util/ArrayList;[I)I", GESHER_NATIVE(AS_listAudioPatches)},
    {"getDevicesForAttributes",
     "(Landroid/media/AudioAttributes;[Landroid/media/AudioDeviceAttributes;Z)I",
     GESHER_NATIVE(AS_getDevicesForAttributes)},
    {"getSupportedDeviceTypes", "(ILandroid/util/IntArray;)I",
     GESHER_NATIVE(AS_getSupportedDeviceTypes)},
};

// --- AudioProductStrategy (the audio policy's engine) --------------------------------------

// Stream type -> the usage and flags of its attributes, as the default
// product strategies (audio_policy_engine_product_strategies.xml) give them.
struct StreamAttributes {
  int usage;
  int flags;
};
constexpr StreamAttributes kStreamAttributes[kNumStreamTypes] = {
    {2, 0},     // VOICE_CALL: VOICE_COMMUNICATION
    {13, 0},    // SYSTEM: ASSISTANCE_SONIFICATION
    {6, 0},     // RING: NOTIFICATION_RINGTONE
    {1, 0},     // MUSIC: MEDIA
    {4, 0},     // ALARM: ALARM
    {5, 0},     // NOTIFICATION: NOTIFICATION
    {2, 0x4},   // BLUETOOTH_SCO: VOICE_COMMUNICATION, FLAG_SCO
    {13, 0x1},  // SYSTEM_ENFORCED: ASSISTANCE_SONIFICATION, FLAG_AUDIBILITY_ENFORCED
    {3, 0},     // DTMF: VOICE_COMMUNICATION_SIGNALLING
    {0, 0x8},   // TTS: FLAG_BEACON
    {11, 0},    // ACCESSIBILITY: ASSISTANCE_ACCESSIBILITY
    {16, 0},    // ASSISTANT: ASSISTANT
};

jint JNICALL APS_getLegacyStreamForAttributes(JNIEnv*, jclass, jobject) {
  // STREAM_DEFAULT: AudioAttributes maps the usage itself (toVolumeStreamType).
  return -1;
}

jint JNICALL APS_getAttributesForLegacyStream(JNIEnv* env, jclass, jint stream, jobject builder) {
  if (builder == nullptr || stream < 0 || stream >= kNumStreamTypes) return kBadValue;
  jclass cls = env->GetObjectClass(builder);
  jfieldID usage = env->GetFieldID(cls, "mUsage", "I");
  jfieldID flags = env->GetFieldID(cls, "mFlags", "I");
  if (usage == nullptr || flags == nullptr) {
    env->ExceptionClear();
    return kError;
  }
  env->SetIntField(builder, usage, kStreamAttributes[stream].usage);
  env->SetIntField(builder, flags, env->GetIntField(builder, flags) | kStreamAttributes[stream].flags);
  return kOk;
}

const JNINativeMethod kProductStrategyMethods[] = {
    {"native_get_legacy_stream_for_audio_attributes", "(Landroid/media/AudioAttributes;)I",
     GESHER_NATIVE(APS_getLegacyStreamForAttributes)},
    {"native_get_audio_attributes_for_legacy_stream", "(ILandroid/media/AudioAttributes$Builder;)I",
     GESHER_NATIVE(APS_getAttributesForLegacyStream)},
};

std::atomic<bool> gAppNamed{false};

}  // namespace

void jniThrow(JNIEnv* env, const char* className, const char* message) {
  jclass cls = env->FindClass(className);
  if (cls != nullptr) env->ThrowNew(cls, message);
}

void EnsureAppName(JNIEnv* env) {
  if (gAppNamed.load()) return;
  jclass at = env->FindClass("android/app/ActivityThread");
  if (at == nullptr) {
    env->ExceptionClear();
    return;
  }
  jmethodID current = env->GetStaticMethodID(at, "currentApplication", "()Landroid/app/Application;");
  jobject app = current != nullptr ? env->CallStaticObjectMethod(at, current) : nullptr;
  if (env->ExceptionCheck() || app == nullptr) {
    env->ExceptionClear();
    return;
  }
  jclass ctx = env->FindClass("android/content/Context");
  jobject info = env->CallObjectMethod(
      app, env->GetMethodID(ctx, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;"));
  jobject pm = env->CallObjectMethod(
      app, env->GetMethodID(ctx, "getPackageManager", "()Landroid/content/pm/PackageManager;"));
  if (env->ExceptionCheck() || info == nullptr || pm == nullptr) {
    env->ExceptionClear();
    return;
  }
  jclass item = env->FindClass("android/content/pm/PackageItemInfo");
  jobject label = env->CallObjectMethod(
      info, env->GetMethodID(item, "loadLabel",
                             "(Landroid/content/pm/PackageManager;)Ljava/lang/CharSequence;"),
      pm);
  jstring s = nullptr;
  if (!env->ExceptionCheck() && label != nullptr) {
    jclass obj = env->FindClass("java/lang/Object");
    s = static_cast<jstring>(
        env->CallObjectMethod(label, env->GetMethodID(obj, "toString", "()Ljava/lang/String;")));
  }
  if (env->ExceptionCheck() || s == nullptr) {
    env->ExceptionClear();
    return;
  }
  SetAppName(Utf16(env, s));
  gAppNamed = true;
}

int LegacyStreamType(JNIEnv* env, jobject attributes) {
  if (attributes == nullptr) return 3;
  jclass cls = env->FindClass("android/media/AudioAttributes");
  jmethodID m = env->GetStaticMethodID(cls, "toLegacyStreamType", "(Landroid/media/AudioAttributes;)I");
  jint stream = m != nullptr ? env->CallStaticIntMethod(cls, m, attributes) : 3;
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return 3;
  }
  return stream >= 0 && stream < kNumStreamTypes ? stream : 3;
}

int RegisterAudioSystem(JNIEnv* env) {
  if (RegisterStandins(env, "android/media/AudioSystem")) {
    RegisterMethods(env, "android/media/AudioSystem", kFrameworkCapabilities);
    RegisterMethods(env, "android/media/AudioSystem", kAudioSystemMethods);
  }
  RegisterStandins(env, "android/media/AudioPortEventHandler");
  if (RegisterStandins(env, "android/media/audiopolicy/AudioProductStrategy")) {
    RegisterMethods(env, "android/media/audiopolicy/AudioProductStrategy", kProductStrategyMethods);
  }
  RegisterStandins(env, "android/media/audiopolicy/AudioVolumeGroup");
  RegisterStandins(env, "android/media/ToneGenerator");
  return 0;
}

}  // namespace gesher::media

// In namespace android, as libandroid_runtime's table declares it.
namespace android {
int register_gesher_media(JNIEnv* env);
}
// The surfaces hwui draws (compat/media/surface/surface_jni.cc, built with hwui).
int register_gesher_media_surfaces(JNIEnv* env);

int android::register_gesher_media(JNIEnv* env) {
  gesher::media::SetVm(env);
  gesher::media::RegisterAudioSystem(env);
  gesher::media::RegisterAudioTrack(env);
  gesher::media::RegisterAudioRecord(env);
  register_gesher_media_surfaces(env);
  return 0;
}
