// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Gesher (devices track, ADR 0094): com.gesher.server.WindowsHello's natives over
// hello_win.h. Registered by register_gesher_devices (devices_jni.cc).
// docs/research/windows-hello-native-spec.md section 3. No callbacks into Java and
// no attached native threads: WindowsHello's own daemon thread blocks in await.

#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>

#include "core_jni_helpers.h"
#include "hello_owner.h"
#include "hello_win.h"

namespace android {

int RegisterGesherNatives(JNIEnv* env, const char* className, const JNINativeMethod* methods,
                          int count);  // services_natives.cc

namespace {

namespace hello = gesher::devices::hello;

// Start's failure reasons, as WindowsHello.java reads out[1].
constexpr jint kReasonNotForeground = 1;
constexpr jint kReasonWindows = 2;

void SetInt(JNIEnv* env, jintArray array, int index, jint value) {
  if (array != nullptr && env->GetArrayLength(array) > index) {
    env->SetIntArrayRegion(array, index, 1, &value);
  }
}

hello::Verification* FromHandle(jlong handle) {
  return reinterpret_cast<hello::Verification*>(static_cast<intptr_t>(handle));
}

jint WindowsHello_availability(JNIEnv* env, jclass, jintArray hrOut) {
  HRESULT hr = S_OK;
  int availability = hello::Availability(&hr);
  SetInt(env, hrOut, 0, static_cast<jint>(hr));
  if (availability < 0) {
    ALOGW("hello: availability query failed: HRESULT 0x%08lX", static_cast<unsigned long>(hr));
  }
  return availability;
}

jint WindowsHello_windowsBuild(JNIEnv*, jclass) {
  return static_cast<jint>(hello::WindowsBuild());
}

jlong WindowsHello_ownerWindow(JNIEnv*, jclass, jint pid) {
  return static_cast<jlong>(
      reinterpret_cast<intptr_t>(hello::OwnerWindow(static_cast<DWORD>(pid))));
}

jint WindowsHello_ownerRelation(JNIEnv*, jclass, jlong owner, jint pid) {
  return static_cast<jint>(hello::ForegroundRelation(
      reinterpret_cast<HWND>(static_cast<intptr_t>(owner)), static_cast<DWORD>(pid)));
}

jlong WindowsHello_start(JNIEnv* env, jclass, jint pid, jstring message, jintArray out) {
  const jchar* chars = message != nullptr ? env->GetStringChars(message, nullptr) : nullptr;
  jsize length = chars != nullptr ? env->GetStringLength(message) : 0;
  hello::StartFailure failure = hello::StartFailure::kNone;
  HRESULT hr = S_OK;
  static const wchar_t kEmpty[] = L"";
  hello::Verification* v = hello::Start(
      static_cast<DWORD>(pid), chars != nullptr ? reinterpret_cast<const wchar_t*>(chars) : kEmpty,
      static_cast<size_t>(length), &failure, &hr);
  if (chars != nullptr) env->ReleaseStringChars(message, chars);
  SetInt(env, out, 0, static_cast<jint>(hr));
  if (v == nullptr) {
    jint reason = failure == hello::StartFailure::kNotForeground ? kReasonNotForeground
                                                                 : kReasonWindows;
    SetInt(env, out, 1, reason);
    ALOGW("hello: verification for pid %d did not start: %s, HRESULT 0x%08lX",
          static_cast<int>(pid), reason == kReasonNotForeground ? "not in the foreground"
                                                                : "Windows refused",
          static_cast<unsigned long>(hr));
    return 0;
  }
  return static_cast<jlong>(reinterpret_cast<intptr_t>(v));
}

jint WindowsHello_await(JNIEnv*, jclass, jlong handle) {
  return handle != 0 ? hello::Await(FromHandle(handle)) : -1;
}

void WindowsHello_cancel(JNIEnv*, jclass, jlong handle) {
  if (handle != 0) hello::Cancel(FromHandle(handle));
}

void WindowsHello_release(JNIEnv*, jclass, jlong handle) {
  if (handle != 0) hello::Release(FromHandle(handle));
}

const JNINativeMethod gMethods[] = {
    {"nativeAvailability", "([I)I", GESHER_JNI(WindowsHello_availability)},
    {"nativeWindowsBuild", "()I", GESHER_JNI(WindowsHello_windowsBuild)},
    {"nativeOwnerWindow", "(I)J", GESHER_JNI(WindowsHello_ownerWindow)},
    {"nativeOwnerRelation", "(JI)I", GESHER_JNI(WindowsHello_ownerRelation)},
    {"nativeStart", "(ILjava/lang/String;[I)J", GESHER_JNI(WindowsHello_start)},
    {"nativeAwait", "(J)I", GESHER_JNI(WindowsHello_await)},
    {"nativeCancel", "(J)V", GESHER_JNI(WindowsHello_cancel)},
    {"nativeRelease", "(J)V", GESHER_JNI(WindowsHello_release)},
};

}  // namespace

int register_gesher_hello(JNIEnv* env) {
  return RegisterGesherNatives(env, "com/gesher/server/WindowsHello", gMethods, NELEM(gMethods));
}

}  // namespace android
