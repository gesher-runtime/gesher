// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Gesher (devices track, ADR 0092): com.gesher.server.WindowsHelloKey's natives over
// hello_key_win.h. Registered by register_gesher_devices (devices_jni.cc). No callbacks
// into Java and no attached native threads: WindowsHelloKey's own thread blocks in await.

#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>

#include <vector>

#include "core_jni_helpers.h"
#include "hello_key_win.h"

namespace android {

int RegisterGesherNatives(JNIEnv* env, const char* className, const JNINativeMethod* methods,
                          int count);  // services_natives.cc

namespace {

namespace hello_key = gesher::devices::hello_key;

void SetInt(JNIEnv* env, jintArray array, int index, jint value) {
  if (array != nullptr && env->GetArrayLength(array) > index) {
    env->SetIntArrayRegion(array, index, 1, &value);
  }
}

// out[0] = the bytes, when there are any and `out` has room.
void SetBytes(JNIEnv* env, jobjectArray out, const std::vector<uint8_t>& bytes) {
  if (out == nullptr || env->GetArrayLength(out) < 1) return;
  jbyteArray array = env->NewByteArray(static_cast<jsize>(bytes.size()));
  if (array == nullptr) return;  // OutOfMemoryError is pending
  env->SetByteArrayRegion(array, 0, static_cast<jsize>(bytes.size()),
                          reinterpret_cast<const jbyte*>(bytes.data()));
  env->SetObjectArrayElement(out, 0, array);
  env->DeleteLocalRef(array);
}

// The UTF-16 of a Java string for one call.
class Chars {
 public:
  Chars(JNIEnv* env, jstring string) : env_(env), string_(string) {
    chars_ = string != nullptr ? env->GetStringChars(string, nullptr) : nullptr;
    length_ = chars_ != nullptr ? static_cast<size_t>(env->GetStringLength(string)) : 0;
  }
  ~Chars() {
    if (chars_ != nullptr) env_->ReleaseStringChars(string_, chars_);
  }
  const wchar_t* get() const {
    return chars_ != nullptr ? reinterpret_cast<const wchar_t*>(chars_) : L"";
  }
  size_t length() const { return length_; }

 private:
  JNIEnv* env_;
  jstring string_;
  const jchar* chars_;
  size_t length_;
};

hello_key::Operation* FromHandle(jlong handle) {
  return reinterpret_cast<hello_key::Operation*>(static_cast<intptr_t>(handle));
}

jint WindowsHelloKey_supported(JNIEnv* env, jclass, jintArray hrOut) {
  HRESULT hr = S_OK;
  int supported = hello_key::Supported(&hr);
  SetInt(env, hrOut, 0, static_cast<jint>(hr));
  if (supported < 0) {
    ALOGW("hello key: IsSupportedAsync failed: HRESULT 0x%08lX", static_cast<unsigned long>(hr));
  }
  return supported;
}

jint WindowsHelloKey_open(JNIEnv* env, jclass, jstring name, jobjectArray publicKeyOut,
                          jintArray hrOut) {
  Chars chars(env, name);
  std::vector<uint8_t> publicKey;
  HRESULT hr = S_OK;
  int status = hello_key::Open(chars.get(), chars.length(), &publicKey, &hr);
  SetInt(env, hrOut, 0, static_cast<jint>(hr));
  if (status == hello_key::kSuccess) SetBytes(env, publicKeyOut, publicKey);
  return status;
}

jint WindowsHelloKey_delete(JNIEnv* env, jclass, jstring name, jintArray hrOut) {
  Chars chars(env, name);
  HRESULT hr = S_OK;
  int status = hello_key::Delete(chars.get(), chars.length(), &hr);
  SetInt(env, hrOut, 0, static_cast<jint>(hr));
  return status;
}

jlong WindowsHelloKey_startCreate(JNIEnv* env, jclass, jint pid, jstring name) {
  Chars chars(env, name);
  return static_cast<jlong>(reinterpret_cast<intptr_t>(
      hello_key::StartCreate(static_cast<DWORD>(pid), chars.get(), chars.length())));
}

jlong WindowsHelloKey_startSign(JNIEnv* env, jclass, jint pid, jstring name, jbyteArray data) {
  Chars chars(env, name);
  std::vector<uint8_t> bytes;
  if (data != nullptr) {
    bytes.resize(static_cast<size_t>(env->GetArrayLength(data)));
    env->GetByteArrayRegion(data, 0, static_cast<jsize>(bytes.size()),
                            reinterpret_cast<jbyte*>(bytes.data()));
  }
  return static_cast<jlong>(reinterpret_cast<intptr_t>(hello_key::StartSign(
      static_cast<DWORD>(pid), chars.get(), chars.length(), bytes.data(), bytes.size())));
}

jint WindowsHelloKey_await(JNIEnv* env, jclass, jlong handle, jobjectArray bytesOut) {
  if (handle == 0) return hello_key::kFailed;
  std::vector<uint8_t> bytes;
  int status = hello_key::Await(FromHandle(handle), &bytes);
  if (status == hello_key::kSuccess) SetBytes(env, bytesOut, bytes);
  return status;
}

void WindowsHelloKey_cancel(JNIEnv*, jclass, jlong handle) {
  if (handle != 0) hello_key::Cancel(FromHandle(handle));
}

void WindowsHelloKey_release(JNIEnv*, jclass, jlong handle) {
  if (handle != 0) hello_key::Release(FromHandle(handle));
}

const JNINativeMethod gMethods[] = {
    {"nativeSupported", "([I)I", GESHER_JNI(WindowsHelloKey_supported)},
    {"nativeOpen", "(Ljava/lang/String;[[B[I)I", GESHER_JNI(WindowsHelloKey_open)},
    {"nativeDelete", "(Ljava/lang/String;[I)I", GESHER_JNI(WindowsHelloKey_delete)},
    {"nativeStartCreate", "(ILjava/lang/String;)J", GESHER_JNI(WindowsHelloKey_startCreate)},
    {"nativeStartSign", "(ILjava/lang/String;[B)J", GESHER_JNI(WindowsHelloKey_startSign)},
    {"nativeAwait", "(J[[B)I", GESHER_JNI(WindowsHelloKey_await)},
    {"nativeCancel", "(J)V", GESHER_JNI(WindowsHelloKey_cancel)},
    {"nativeRelease", "(J)V", GESHER_JNI(WindowsHelloKey_release)},
};

}  // namespace

int register_gesher_hello_key(JNIEnv* env) {
  return RegisterGesherNatives(env, "com/gesher/server/WindowsHelloKey", gMethods,
                               NELEM(gMethods));
}

}  // namespace android
