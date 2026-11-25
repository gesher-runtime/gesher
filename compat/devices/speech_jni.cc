// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Gesher (devices track, docs/tts.md): com.gesher.server.WindowsSpeech's natives over
// speech_win.h. Registered by register_gesher_devices (devices_jni.cc), so every
// process of the device has them: the system server (which voices exist) and the
// Gesher text-to-speech engine's process (app/gesher-tts). No callbacks into Java
// and no attached native threads: the engine's synthesis thread blocks in await.

#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>

#include <string>
#include <vector>

#include "core_jni_helpers.h"
#include "speech_win.h"

namespace android {

int RegisterGesherNatives(JNIEnv* env, const char* className, const JNINativeMethod* methods,
                          int count);  // services_natives.cc

namespace {

namespace speech = gesher::devices::speech;

// Start's failure reasons, as WindowsSpeech.java reads out[1].
constexpr jint kReasonWindows = 1;
constexpr jint kReasonNoSuchVoice = 2;
constexpr jint kReasonProsody = 3;

// Fields per voice in nativeVoices' array, as WindowsSpeech.voicesOf reads them.
constexpr int kVoiceFields = 5;

void SetInt(JNIEnv* env, jintArray array, int index, jint value) {
  if (array != nullptr && env->GetArrayLength(array) > index) {
    env->SetIntArrayRegion(array, index, 1, &value);
  }
}

speech::Synthesis* FromHandle(jlong handle) {
  return reinterpret_cast<speech::Synthesis*>(static_cast<intptr_t>(handle));
}

jstring NewString(JNIEnv* env, const std::wstring& s) {
  return env->NewString(reinterpret_cast<const jchar*>(s.data()), static_cast<jsize>(s.size()));
}

std::wstring StringOf(JNIEnv* env, jstring s) {
  if (s == nullptr) return std::wstring();
  const jchar* chars = env->GetStringChars(s, nullptr);
  if (chars == nullptr) return std::wstring();
  std::wstring value(reinterpret_cast<const wchar_t*>(chars), env->GetStringLength(s));
  env->ReleaseStringChars(s, chars);
  return value;
}

// [id, displayName, language, gender ("0" male, "1" female), default ("1"/"0")] per
// voice; null when Windows has no speech synthesis (hrOut[0] says why).
jobjectArray WindowsSpeech_voices(JNIEnv* env, jclass, jintArray hrOut) {
  std::vector<speech::Voice> voices;
  HRESULT hr = S_OK;
  bool ok = speech::Voices(&voices, &hr);
  SetInt(env, hrOut, 0, static_cast<jint>(hr));
  if (!ok) {
    ALOGW("speech: no Windows speech synthesis: HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return nullptr;
  }
  jclass string = env->FindClass("java/lang/String");
  if (string == nullptr) return nullptr;
  jobjectArray result =
      env->NewObjectArray(static_cast<jsize>(voices.size() * kVoiceFields), string, nullptr);
  env->DeleteLocalRef(string);
  if (result == nullptr) return nullptr;
  jsize i = 0;
  for (const speech::Voice& v : voices) {
    const std::wstring fields[kVoiceFields] = {v.id, v.displayName, v.language,
                                               v.gender == 1 ? L"1" : L"0",
                                               v.isDefault ? L"1" : L"0"};
    for (const std::wstring& field : fields) {
      jstring s = NewString(env, field);
      if (s == nullptr) return nullptr;
      env->SetObjectArrayElement(result, i++, s);
      env->DeleteLocalRef(s);
    }
  }
  return result;
}

jlong WindowsSpeech_start(JNIEnv* env, jclass, jstring text, jstring voiceId, jdouble rate,
                          jdouble pitch, jboolean words, jintArray out) {
  if (text == nullptr || voiceId == nullptr) {
    jniThrowNullPointerException(env, "text and voiceId");
    return 0;
  }
  std::wstring input = StringOf(env, text);
  std::wstring voice = StringOf(env, voiceId);
  speech::StartFailure failure = speech::StartFailure::kNone;
  HRESULT hr = S_OK;
  speech::Synthesis* s = speech::Start(input.data(), input.size(), voice.c_str(), rate, pitch,
                                       words == JNI_TRUE, &failure, &hr);
  SetInt(env, out, 0, static_cast<jint>(hr));
  if (s == nullptr) {
    jint reason = failure == speech::StartFailure::kNoSuchVoice    ? kReasonNoSuchVoice
                  : failure == speech::StartFailure::kProsodyUnsupported ? kReasonProsody
                                                                         : kReasonWindows;
    SetInt(env, out, 1, reason);
    ALOGW("speech: synthesis did not start: reason %d, HRESULT 0x%08lX", static_cast<int>(reason),
          static_cast<unsigned long>(hr));
    return 0;
  }
  return static_cast<jlong>(reinterpret_cast<intptr_t>(s));
}

jint WindowsSpeech_await(JNIEnv* env, jclass, jlong handle, jlong timeoutMs, jintArray hrOut) {
  if (handle == 0) return speech::kFailed;
  HRESULT hr = S_OK;
  int status = speech::Await(FromHandle(handle), timeoutMs, &hr);
  SetInt(env, hrOut, 0, static_cast<jint>(hr));
  return status;
}

jbyteArray WindowsSpeech_audio(JNIEnv* env, jclass, jlong handle) {
  if (handle == 0) return nullptr;
  const std::vector<uint8_t>& audio = speech::Audio(FromHandle(handle));
  jbyteArray result = env->NewByteArray(static_cast<jsize>(audio.size()));
  if (result != nullptr && !audio.empty()) {
    env->SetByteArrayRegion(result, 0, static_cast<jsize>(audio.size()),
                            reinterpret_cast<const jbyte*>(audio.data()));
  }
  return result;
}

// [startTicks, first, last] per word.
jlongArray WindowsSpeech_words(JNIEnv* env, jclass, jlong handle) {
  if (handle == 0) return nullptr;
  const std::vector<speech::Word>& words = speech::Words(FromHandle(handle));
  std::vector<jlong> flat;
  flat.reserve(words.size() * 3);
  for (const speech::Word& w : words) {
    flat.push_back(w.startTicks);
    flat.push_back(w.first);
    flat.push_back(w.last);
  }
  jlongArray result = env->NewLongArray(static_cast<jsize>(flat.size()));
  if (result != nullptr && !flat.empty()) {
    env->SetLongArrayRegion(result, 0, static_cast<jsize>(flat.size()), flat.data());
  }
  return result;
}

void WindowsSpeech_cancel(JNIEnv*, jclass, jlong handle) {
  if (handle != 0) speech::Cancel(FromHandle(handle));
}

void WindowsSpeech_release(JNIEnv*, jclass, jlong handle) {
  if (handle != 0) speech::Release(FromHandle(handle));
}

const JNINativeMethod gMethods[] = {
    {"nativeVoices", "([I)[Ljava/lang/String;", GESHER_JNI(WindowsSpeech_voices)},
    {"nativeStart", "(Ljava/lang/String;Ljava/lang/String;DDZ[I)J",
     GESHER_JNI(WindowsSpeech_start)},
    {"nativeAwait", "(JJ[I)I", GESHER_JNI(WindowsSpeech_await)},
    {"nativeAudio", "(J)[B", GESHER_JNI(WindowsSpeech_audio)},
    {"nativeWords", "(J)[J", GESHER_JNI(WindowsSpeech_words)},
    {"nativeCancel", "(J)V", GESHER_JNI(WindowsSpeech_cancel)},
    {"nativeRelease", "(J)V", GESHER_JNI(WindowsSpeech_release)},
};

}  // namespace

int register_gesher_speech(JNIEnv* env) {
  return RegisterGesherNatives(env, "com/gesher/server/WindowsSpeech", gMethods, NELEM(gMethods));
}

}  // namespace android
