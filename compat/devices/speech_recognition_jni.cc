// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Gesher (devices track, docs/speech-recognition.md): com.gesher.server.
// WindowsSpeechRecognition's natives over speech_recognition_win.h. Registered by
// register_gesher_devices (devices_jni.cc), so every process of the device has them:
// the system server (whether Windows has recognizers, SttEngineApp) and the Gesher
// speech recognition engine's process (app/gesher-stt). No callbacks into Java and no
// attached native threads: the engine's session thread writes PCM and reads events.

#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>

#include <memory>
#include <string>
#include <vector>

#include "core_jni_helpers.h"
#include "speech_recognition_win.h"

namespace android {

int RegisterGesherNatives(JNIEnv* env, const char* className, const JNINativeMethod* methods,
                          int count);  // services_natives.cc

namespace {

namespace sr = gesher::devices::speech_recognition;

// nativeStart's failure reasons, as WindowsSpeechRecognition.java reads out[1].
constexpr jint kReasonWindows = 1;
constexpr jint kReasonNoRecognizer = 2;
constexpr jint kReasonRecognizerFailed = 3;

// Fields per recognizer in nativeRecognizers' array, as WindowsSpeechRecognition reads them.
constexpr int kRecognizerFields = 4;

void SetInt(JNIEnv* env, jintArray array, int index, jint value) {
  if (array != nullptr && env->GetArrayLength(array) > index) {
    env->SetIntArrayRegion(array, index, 1, &value);
  }
}

sr::Session* FromHandle(jlong handle) {
  return reinterpret_cast<sr::Session*>(static_cast<intptr_t>(handle));
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

// [tokenId, name, languages (BCP-47, comma-separated), default ("1"/"0")] per
// recognizer; null when SAPI cannot be used (hrOut[0] says why). An empty array: none.
jobjectArray WSR_nativeRecognizers(JNIEnv* env, jclass, jintArray hrOut) {
  std::vector<sr::Recognizer> recognizers;
  HRESULT hr = sr::EnumerateRecognizers(&recognizers);
  SetInt(env, hrOut, 0, static_cast<jint>(hr));
  if (FAILED(hr)) {
    ALOGW("SAPI recognizers: HRESULT 0x%08lx", static_cast<unsigned long>(hr));
    return nullptr;
  }
  jclass string_class = env->FindClass("java/lang/String");
  if (string_class == nullptr) return nullptr;
  jobjectArray out = env->NewObjectArray(
      static_cast<jsize>(recognizers.size() * kRecognizerFields), string_class, nullptr);
  env->DeleteLocalRef(string_class);
  if (out == nullptr) return nullptr;
  jsize i = 0;
  for (const sr::Recognizer& r : recognizers) {
    std::wstring languages;
    for (const std::wstring& l : r.languages) languages += (languages.empty() ? L"" : L",") + l;
    const std::wstring fields[kRecognizerFields] = {r.token_id, r.name, languages,
                                                   r.is_default ? L"1" : L"0"};
    for (const std::wstring& field : fields) {
      jstring s = NewString(env, field);
      if (s == nullptr) return nullptr;
      env->SetObjectArrayElement(out, i++, s);
      env->DeleteLocalRef(s);
    }
  }
  return out;
}

// A session handle, or 0 with out[0] = HRESULT and out[1] = the reason.
jlong WSR_nativeStart(JNIEnv* env, jclass, jstring language, jint sampleRate,
                      jboolean hypotheses, jint maxAlternates, jintArray out) {
  sr::SessionOptions options;
  options.language = StringOf(env, language);
  options.sample_rate = sampleRate;
  options.hypotheses = hypotheses == JNI_TRUE;
  options.max_alternates = maxAlternates;
  std::unique_ptr<sr::Session> session;
  bool recognizer_failed = false;
  HRESULT hr = sr::StartSession(options, &session, &recognizer_failed);
  SetInt(env, out, 0, static_cast<jint>(hr));
  if (FAILED(hr)) {
    SetInt(env, out, 1, hr == sr::kSpErrNotFound ? kReasonNoRecognizer
                        : recognizer_failed     ? kReasonRecognizerFailed
                                                : kReasonWindows);
    return 0;
  }
  return static_cast<jlong>(reinterpret_cast<intptr_t>(session.release()));
}

jboolean WSR_nativeWrite(JNIEnv* env, jclass, jlong handle, jshortArray pcm, jint count) {
  sr::Session* session = FromHandle(handle);
  if (session == nullptr || pcm == nullptr || count < 0 || count > env->GetArrayLength(pcm)) {
    return JNI_FALSE;
  }
  std::vector<int16_t> samples(static_cast<size_t>(count));
  env->GetShortArrayRegion(pcm, 0, count, reinterpret_cast<jshort*>(samples.data()));
  return session->Write(samples.data(), samples.size()) ? JNI_TRUE : JNI_FALSE;
}

void WSR_nativeEndAudio(JNIEnv*, jclass, jlong handle) {
  if (sr::Session* session = FromHandle(handle)) session->EndAudio();
}

void WSR_nativeCancel(JNIEnv*, jclass, jlong handle) {
  if (sr::Session* session = FromHandle(handle)) session->Cancel();
}

void WSR_nativeRelease(JNIEnv*, jclass, jlong handle) { delete FromHandle(handle); }

// The next event's kind (speech_recognition_win.h's EventKind values). For a
// recognition or hypothesis: out[0] = String[] texts, out[1] = float[] confidences.
// For a failure: hrOut[0] = its HRESULT.
jint WSR_nativeNextEvent(JNIEnv* env, jclass, jlong handle, jint timeoutMs, jobjectArray out,
                         jintArray hrOut) {
  sr::Session* session = FromHandle(handle);
  if (session == nullptr) return static_cast<jint>(sr::EventKind::kNone);
  sr::Event e = session->NextEvent(timeoutMs < 0 ? 0 : static_cast<DWORD>(timeoutMs));
  SetInt(env, hrOut, 0, static_cast<jint>(e.hr));
  if (!e.texts.empty() && out != nullptr && env->GetArrayLength(out) >= 2) {
    jclass string_class = env->FindClass("java/lang/String");
    if (string_class == nullptr) return static_cast<jint>(sr::EventKind::kNone);
    jobjectArray texts =
        env->NewObjectArray(static_cast<jsize>(e.texts.size()), string_class, nullptr);
    env->DeleteLocalRef(string_class);
    jfloatArray confidences = env->NewFloatArray(static_cast<jsize>(e.confidences.size()));
    if (texts == nullptr || confidences == nullptr) return static_cast<jint>(sr::EventKind::kNone);
    for (size_t i = 0; i < e.texts.size(); ++i) {
      jstring s = NewString(env, e.texts[i]);
      if (s == nullptr) return static_cast<jint>(sr::EventKind::kNone);
      env->SetObjectArrayElement(texts, static_cast<jsize>(i), s);
      env->DeleteLocalRef(s);
    }
    env->SetFloatArrayRegion(confidences, 0, static_cast<jsize>(e.confidences.size()),
                             e.confidences.data());
    env->SetObjectArrayElement(out, 0, texts);
    env->SetObjectArrayElement(out, 1, confidences);
    env->DeleteLocalRef(texts);
    env->DeleteLocalRef(confidences);
  }
  return static_cast<jint>(e.kind);
}

const JNINativeMethod gMethods[] = {
    {"nativeRecognizers", "([I)[Ljava/lang/String;", GESHER_JNI(WSR_nativeRecognizers)},
    {"nativeStart", "(Ljava/lang/String;IZI[I)J", GESHER_JNI(WSR_nativeStart)},
    {"nativeWrite", "(J[SI)Z", GESHER_JNI(WSR_nativeWrite)},
    {"nativeEndAudio", "(J)V", GESHER_JNI(WSR_nativeEndAudio)},
    {"nativeCancel", "(J)V", GESHER_JNI(WSR_nativeCancel)},
    {"nativeRelease", "(J)V", GESHER_JNI(WSR_nativeRelease)},
    {"nativeNextEvent", "(JI[Ljava/lang/Object;[I)I", GESHER_JNI(WSR_nativeNextEvent)},
};

}  // namespace

int register_gesher_speech_recognition(JNIEnv* env) {
  return RegisterGesherNatives(env, "com/gesher/server/WindowsSpeechRecognition", gMethods,
                               NELEM(gMethods));
}

}  // namespace android
