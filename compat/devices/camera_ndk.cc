// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// libcamera2ndk discovery and device ownership over the authoritative Camera2 service.

#include <jni.h>
#include <nativehelper/scoped_local_ref.h>
#include <nativehelper/scoped_utf_chars.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

#include <gesher/bionic_shim.h>

#include "../arm64/include/gesher/arm64_runtime.h"
#include "../bionic/abi_types.h"
#include "../elf/include/gesher/elf_loader.h"
#include "../media/jni/jni_util.h"

// The public NDK layouts (NdkCameraManager.h and NdkCameraDevice.h). The manager is opaque, and
// each discovery result owns its pointer array and NUL-terminated IDs.
struct ACameraManager {};
struct ACameraIdList {
  int numCameras;
  const char** cameraIds;
};

struct ACameraDevice;
using DisconnectedCallback = void (BIONIC_SYSV*)(void*, ACameraDevice*);
using ErrorCallback = void (BIONIC_SYSV*)(void*, ACameraDevice*, int);

// Passed by pointer on both guest ABIs.
struct ACameraDevice_StateCallbacks {
  void* context;
  DisconnectedCallback disconnected;
  ErrorCallback error;
};

struct ACameraDevice {
  std::unique_ptr<char[]> id;
  jobject peer = nullptr;
  ACameraDevice_StateCallbacks callback{};
  void* disconnect_pc = nullptr;
  void* error_pc = nullptr;
  gesher::elf::Library* disconnect_owner = nullptr;
  gesher::elf::Library* error_owner = nullptr;

  ~ACameraDevice() {
    gesher::elf::ReleaseLifetimePin(disconnect_owner);
    gesher::elf::ReleaseLifetimePin(error_owner);
  }
};

namespace android {
namespace {

// NdkCameraError.h.
constexpr int kOk = 0;
constexpr int kUnknown = -10000;
constexpr int kInvalidParameter = -10001;
constexpr int kDisconnected = -10002;
constexpr int kNotEnoughMemory = -10003;
constexpr int kCameraInUse = -10010;
constexpr int kMaxCamerasInUse = -10011;
constexpr int kDisabled = -10012;
constexpr int kPermissionDenied = -10013;

jclass gCameraManager;
jmethodID gGetCameraIdList;
jmethodID gOpenCamera;
jclass gDevice;
jmethodID gDeviceConstructor;
jmethodID gPublishDevice;
jmethodID gCloseDevice;

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

// ICameraService's service-specific errors to public NDK status values.
int OpenStatus(int status) {
  switch (status) {
    case 0: return kOk;
    case 1: return kPermissionDenied;
    case 3:
    case 9: return kInvalidParameter;
    case 4: return kDisconnected;
    case 6: return kDisabled;
    case 7: return kCameraInUse;
    case 8: return kMaxCamerasInUse;
    default: return kUnknown;
  }
}

template <typename Callback>
bool ResolveCallback(Callback* callback, void** pc, gesher::elf::Library** owner) {
  *pc = reinterpret_cast<void*>(*callback);
  if (*pc == nullptr) return true;
  *owner = gesher::elf::RetainForAddress(*pc);
  if (!gesher::arm64::IsGuestCode(*pc)) return true;
  *callback = reinterpret_cast<Callback>(gesher::arm64::HostEntry(*pc));
  return *callback != nullptr;
}

int BIONIC_SYSV OpenCamera(ACameraManager* manager, const char* id,
                         const ACameraDevice_StateCallbacks* callbacks, ACameraDevice** out) {
  if (manager == nullptr || id == nullptr || callbacks == nullptr || out == nullptr) {
    return kInvalidParameter;
  }
  JNIEnv* env = gesher::media::AttachedEnv("gesher-camera-ndk");
  if (env == nullptr || env->ExceptionCheck()) return kDisconnected;
  auto device = std::unique_ptr<ACameraDevice>(new (std::nothrow) ACameraDevice);
  if (!device) return kNotEnoughMemory;
  const size_t id_length = std::strlen(id) + 1;
  device->id.reset(new (std::nothrow) char[id_length]);
  if (!device->id) return kNotEnoughMemory;
  std::memcpy(device->id.get(), id, id_length);
  device->callback = *callbacks;
  if (!ResolveCallback(&device->callback.disconnected, &device->disconnect_pc,
                       &device->disconnect_owner) ||
      !ResolveCallback(&device->callback.error, &device->error_pc, &device->error_owner)) {
    return kUnknown;
  }

  ScopedLocalRef<jstring> java_id(env, env->NewStringUTF(id));
  if (java_id.get() == nullptr) return QueryFailure(env);
  ScopedLocalRef<jobject> peer(env, env->NewObject(gDevice, gDeviceConstructor));
  if (peer.get() == nullptr) return QueryFailure(env);
  device->peer = env->NewGlobalRef(peer.get());
  if (device->peer == nullptr) return QueryFailure(env);
  const jint result = env->CallStaticIntMethod(gCameraManager, gOpenCamera, java_id.get(), peer.get());
  const int status = env->ExceptionCheck() ? QueryFailure(env) : OpenStatus(result);
  if (status != kOk) {
    env->CallVoidMethod(peer.get(), gCloseDevice);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteGlobalRef(device->peer);
    return status;
  }

  // A service callback can close this handle as soon as publish wakes it.
  ACameraDevice* published = device.release();
  *out = published;
  env->CallVoidMethod(peer.get(), gPublishDevice, static_cast<jlong>(reinterpret_cast<intptr_t>(published)));
  return kOk;
}

int BIONIC_SYSV CloseCamera(ACameraDevice* device) {
  if (device == nullptr) return kInvalidParameter;
  JNIEnv* env = gesher::media::AttachedEnv("gesher-camera-ndk");
  if (env == nullptr || env->ExceptionCheck()) return kDisconnected;
  env->CallVoidMethod(device->peer, gCloseDevice);
  // The Java bridge retired the callback before disconnecting the service peer.
  // A service that has already disappeared needs no further teardown.
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteGlobalRef(device->peer);
  delete device;
  return kOk;
}

const char* BIONIC_SYSV GetCameraId(const ACameraDevice* device) {
  return device != nullptr ? device->id.get() : nullptr;
}

void BIONIC_SYSV DispatchTerminal(JNIEnv*, jclass, jlong pointer, jint error) {
  auto* device = reinterpret_cast<ACameraDevice*>(static_cast<intptr_t>(pointer));
  const auto callbacks = device->callback;
  // Keep guest code alive even if the callback calls ACameraDevice_close.
  auto pin = std::unique_ptr<gesher::elf::Library, decltype(&gesher::elf::ReleaseLifetimePin)>(
      gesher::elf::RetainForAddress(error == 0 ? device->disconnect_pc : device->error_pc),
      gesher::elf::ReleaseLifetimePin);
  gesher::bionic::EnterLoadedCode();
  if (error == 0) {
    if (callbacks.disconnected != nullptr) callbacks.disconnected(callbacks.context, device);
  } else if (callbacks.error != nullptr) {
    callbacks.error(callbacks.context, device, error);
  }
}

void RegisterDevice(JNIEnv* env, jclass bridge) {
  ScopedLocalRef<jclass> device(env, env->FindClass("android/app/GesherCameraManager$Device"));
  if (device.get() == nullptr) {
    env->ExceptionClear();
    return;
  }
  gOpenCamera = env->GetStaticMethodID(bridge, "openCamera",
      "(Ljava/lang/String;Landroid/app/GesherCameraManager$Device;)I");
  if (gOpenCamera == nullptr) {
    env->ExceptionClear();
    return;
  }
  gDeviceConstructor = env->GetMethodID(device.get(), "<init>", "()V");
  if (gDeviceConstructor == nullptr) {
    env->ExceptionClear();
    return;
  }
  gPublishDevice = env->GetMethodID(device.get(), "publish", "(J)V");
  if (gPublishDevice == nullptr) {
    env->ExceptionClear();
    return;
  }
  gCloseDevice = env->GetMethodID(device.get(), "close", "()V");
  if (gCloseDevice == nullptr) {
    env->ExceptionClear();
    return;
  }
  const JNINativeMethod callback = {"dispatchTerminal", "(JI)V",
                                   reinterpret_cast<void*>(&DispatchTerminal)};
  if (env->RegisterNatives(bridge, &callback, 1) != JNI_OK) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    return;
  }
  gDevice = static_cast<jclass>(env->NewGlobalRef(device.get()));
  if (gDevice == nullptr) {
    env->ExceptionClear();
    return;
  }
  using gesher::bionic::AddBionicSymbol;
  AddBionicSymbol("ACameraManager_openCamera", reinterpret_cast<void*>(&OpenCamera));
  AddBionicSymbol("ACameraDevice_close", reinterpret_cast<void*>(&CloseCamera));
  AddBionicSymbol("ACameraDevice_getId", reinterpret_cast<void*>(&GetCameraId));
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
  RegisterDevice(env, bridge.get());
}

}  // namespace android
