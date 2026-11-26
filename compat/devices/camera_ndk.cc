// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// libcamera2ndk discovery, over the same Camera2 service as the Java API.

#include <jni.h>
#include <nativehelper/scoped_local_ref.h>
#include <nativehelper/scoped_utf_chars.h>

#include <cstring>
#include <memory>
#include <new>

#include <gesher/bionic_shim.h>

#include "../bionic/abi_types.h"
#include "../media/jni/jni_util.h"

// The public NDK layouts (NdkCameraManager.h and NdkCameraDevice.h). The manager is opaque, and
// each discovery result owns its pointer array and NUL-terminated IDs.
struct ACameraManager {};
struct ACameraIdList {
  int numCameras;
  const char** cameraIds;
};

namespace android {
namespace {

// NdkCameraError.h.
constexpr int kOk = 0;
constexpr int kInvalidParameter = -10001;
constexpr int kDisconnected = -10002;
constexpr int kNotEnoughMemory = -10003;
constexpr int kPermissionDenied = -10013;

jclass gCameraManager;
jmethodID gGetCameraIdList;

ACameraManager* BIONIC_SYSV CreateManager() {
  return new (std::nothrow) ACameraManager;
}

void BIONIC_SYSV DeleteManager(ACameraManager* manager) {
  delete manager;
}

void BIONIC_SYSV DeleteIdList(ACameraIdList* ids) {
  if (ids == nullptr) return;
  for (int i = 0; i < ids->numCameras; ++i) delete[] ids->cameraIds[i];
  delete[] ids->cameraIds;
  delete ids;
}

int QueryFailure(JNIEnv* env) {
  ScopedLocalRef<jthrowable> failure(env, env->ExceptionOccurred());
  if (failure.get() == nullptr) return kDisconnected;
  env->ExceptionClear();
  ScopedLocalRef<jclass> oom(env, env->FindClass("java/lang/OutOfMemoryError"));
  if (oom.get() != nullptr && env->IsInstanceOf(failure.get(), oom.get())) {
    return kNotEnoughMemory;
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  ScopedLocalRef<jclass> denied(env, env->FindClass("java/lang/SecurityException"));
  if (denied.get() != nullptr && env->IsInstanceOf(failure.get(), denied.get())) {
    return kPermissionDenied;
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  return kDisconnected;
}

int BIONIC_SYSV GetCameraIdList(ACameraManager* manager, ACameraIdList** out) {
  if (manager == nullptr || out == nullptr) return kInvalidParameter;
  JNIEnv* env = gesher::media::AttachedEnv("gesher-camera-ndk");
  if (env == nullptr) return kDisconnected;
  // A guest can call this API while it has an unrelated pending JNI exception.
  // Leave that exception for its caller.
  if (env->ExceptionCheck()) return kDisconnected;
  ScopedLocalRef<jobjectArray> java_ids(
      env, static_cast<jobjectArray>(env->CallStaticObjectMethod(gCameraManager, gGetCameraIdList)));
  if (java_ids.get() == nullptr) return QueryFailure(env);

  const jsize count = env->GetArrayLength(java_ids.get());
  std::unique_ptr<ACameraIdList, decltype(&DeleteIdList)> ids(
      new (std::nothrow) ACameraIdList{}, DeleteIdList);
  if (!ids) return kNotEnoughMemory;
  ids->cameraIds = new (std::nothrow) const char*[count]();
  if (ids->cameraIds == nullptr) return kNotEnoughMemory;
  ids->numCameras = count;
  for (jsize i = 0; i < count; ++i) {
    ScopedLocalRef<jstring> id(env, static_cast<jstring>(env->GetObjectArrayElement(java_ids.get(), i)));
    if (id.get() == nullptr) return QueryFailure(env);
    ScopedUtfChars utf(env, id.get());
    if (utf.c_str() == nullptr) return QueryFailure(env);
    const size_t length = std::strlen(utf.c_str()) + 1;
    auto* copy = new (std::nothrow) char[length];
    if (copy == nullptr) return kNotEnoughMemory;
    std::memcpy(copy, utf.c_str(), length);
    ids->cameraIds[i] = copy;
  }
  *out = ids.release();
  return kOk;
}

}  // namespace

void register_gesher_camera_ndk(JNIEnv* env) {
  if (gCameraManager != nullptr) return;
  ScopedLocalRef<jclass> bridge(env, env->FindClass("android/app/GesherCameraManager"));
  if (bridge.get() == nullptr) {
    env->ExceptionClear();
    return;
  }
  gGetCameraIdList = env->GetStaticMethodID(bridge.get(), "getCameraIdList", "()[Ljava/lang/String;");
  if (gGetCameraIdList == nullptr) {
    env->ExceptionClear();
    return;
  }
  gCameraManager = static_cast<jclass>(env->NewGlobalRef(bridge.get()));
  if (gCameraManager == nullptr) {
    env->ExceptionClear();
    return;
  }
  gesher::media::SetVm(env);
  using gesher::bionic::AddBionicSymbol;
  AddBionicSymbol("ACameraManager_create", reinterpret_cast<void*>(&CreateManager));
  AddBionicSymbol("ACameraManager_delete", reinterpret_cast<void*>(&DeleteManager));
  AddBionicSymbol("ACameraManager_getCameraIdList", reinterpret_cast<void*>(&GetCameraIdList));
  AddBionicSymbol("ACameraManager_deleteCameraIdList", reinterpret_cast<void*>(&DeleteIdList));
}

}  // namespace android
