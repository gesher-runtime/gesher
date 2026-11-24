// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Gesher (devices track, ADR 0092): com.gesher.server.WindowsWebAuthn's natives
// over webauthn_win.h (docs/passkeys.md). Registered by register_gesher_devices
// (devices_jni.cc). No callbacks into Java: the calling Java thread blocks in a
// ceremony while Windows shows its dialog.

#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>

#include <string.h>

#include <string>
#include <vector>

#include "core_jni_helpers.h"
#include "webauthn_win.h"

namespace android {

int RegisterGesherNatives(JNIEnv* env, const char* className, const JNINativeMethod* methods,
                          int count);  // services_natives.cc

namespace {

namespace webauthn = gesher::devices::webauthn;

std::wstring WideString(JNIEnv* env, jstring string) {
  if (string == nullptr) return std::wstring();
  const jchar* chars = env->GetStringChars(string, nullptr);
  if (chars == nullptr) return std::wstring();
  std::wstring wide(reinterpret_cast<const wchar_t*>(chars),
                    static_cast<size_t>(env->GetStringLength(string)));
  env->ReleaseStringChars(string, chars);
  return wide;
}

std::vector<uint8_t> ByteVector(JNIEnv* env, jbyteArray array) {
  std::vector<uint8_t> bytes;
  if (array == nullptr) return bytes;
  jsize length = env->GetArrayLength(array);
  bytes.resize(static_cast<size_t>(length));
  if (length > 0) {
    env->GetByteArrayRegion(array, 0, length, reinterpret_cast<jbyte*>(bytes.data()));
  }
  return bytes;
}

std::vector<webauthn::Descriptor> Descriptors(JNIEnv* env, jobjectArray ids,
                                              jintArray transports) {
  std::vector<webauthn::Descriptor> descriptors;
  if (ids == nullptr) return descriptors;
  jsize count = env->GetArrayLength(ids);
  jsize transport_count = transports != nullptr ? env->GetArrayLength(transports) : 0;
  for (jsize i = 0; i < count; i++) {
    jbyteArray id = static_cast<jbyteArray>(env->GetObjectArrayElement(ids, i));
    webauthn::Descriptor descriptor;
    descriptor.id = ByteVector(env, id);
    if (id != nullptr) env->DeleteLocalRef(id);
    if (i < transport_count) {
      jint bits = 0;
      env->GetIntArrayRegion(transports, i, 1, &bits);
      descriptor.transports = static_cast<DWORD>(bits);
    }
    descriptors.push_back(descriptor);
  }
  return descriptors;
}

void SetInt(JNIEnv* env, jintArray array, jsize index, jint value) {
  if (array != nullptr && env->GetArrayLength(array) > index) {
    env->SetIntArrayRegion(array, index, 1, &value);
  }
}

void SetBytes(JNIEnv* env, jobjectArray out, jsize index, const std::vector<uint8_t>& bytes) {
  if (out == nullptr || env->GetArrayLength(out) <= index) return;
  jsize length = static_cast<jsize>(bytes.size());
  jbyteArray array = env->NewByteArray(length);
  if (array == nullptr) return;  // OutOfMemoryError is pending
  if (length > 0) {
    env->SetByteArrayRegion(array, 0, length, reinterpret_cast<const jbyte*>(bytes.data()));
  }
  env->SetObjectArrayElement(out, index, array);
  env->DeleteLocalRef(array);
}

// Windows' name for a failure (WebAuthNGetErrorName), for the Java error mapping.
void SetErrorName(JNIEnv* env, jobjectArray out, jsize index, HRESULT hr) {
  if (out == nullptr || env->GetArrayLength(out) <= index) return;
  std::wstring name = webauthn::ErrorName(hr);
  if (name.empty()) return;
  jstring string = env->NewString(reinterpret_cast<const jchar*>(name.data()),
                                  static_cast<jsize>(name.size()));
  if (string == nullptr) return;
  env->SetObjectArrayElement(out, index, string);
  env->DeleteLocalRef(string);
}

// The cancellation id as Java holds it: the GUID's 16 bytes.
bool Cancellation(JNIEnv* env, jbyteArray id, GUID* guid) {
  if (id == nullptr || env->GetArrayLength(id) != static_cast<jsize>(sizeof(GUID))) return false;
  env->GetByteArrayRegion(id, 0, static_cast<jsize>(sizeof(GUID)),
                          reinterpret_cast<jbyte*>(guid));
  return true;
}

jint WindowsWebAuthn_apiVersion(JNIEnv*, jclass) {
  return static_cast<jint>(webauthn::ApiVersion());
}

jbyteArray WindowsWebAuthn_newCancellation(JNIEnv* env, jclass) {
  GUID guid{};
  HRESULT hr = webauthn::GetCancellationId(&guid);
  if (FAILED(hr)) {
    ALOGW("webauthn: no cancellation id: HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return nullptr;
  }
  jbyteArray id = env->NewByteArray(static_cast<jsize>(sizeof(GUID)));
  if (id != nullptr) {
    env->SetByteArrayRegion(id, 0, static_cast<jsize>(sizeof(GUID)),
                            reinterpret_cast<const jbyte*>(&guid));
  }
  return id;
}

jint WindowsWebAuthn_cancel(JNIEnv* env, jclass, jbyteArray id) {
  GUID guid{};
  if (!Cancellation(env, id, &guid)) return static_cast<jint>(E_INVALIDARG);
  return static_cast<jint>(webauthn::CancelCurrentOperation(guid));
}

// info: [0] 1 when the caller does not own the foreground window, [1] the used
// transport, [2] the authenticator's transports or -1, [3] bResidentKey or -1.
// out: [0] credential id, [1] authenticator data, [2] attestation object,
// [3] Windows' error name.
jint WindowsWebAuthn_makeCredential(JNIEnv* env, jclass, jint pid, jstring rpId, jstring rpName,
                                    jbyteArray userId, jstring userName,
                                    jstring userDisplayName, jintArray algorithms,
                                    jbyteArray clientDataJson, jint timeoutMs,
                                    jobjectArray excludeIds, jintArray excludeTransports,
                                    jint attachment, jint residentKey, jint userVerification,
                                    jint attestation, jbyteArray cancellation, jobjectArray out,
                                    jintArray info) {
  webauthn::MakeCredentialRequest request;
  request.rp_id = WideString(env, rpId);
  request.rp_name = WideString(env, rpName);
  request.user_id = ByteVector(env, userId);
  request.user_name = WideString(env, userName);
  request.user_display_name = WideString(env, userDisplayName);
  if (algorithms != nullptr) {
    jsize count = env->GetArrayLength(algorithms);
    std::vector<jint> values(static_cast<size_t>(count));
    if (count > 0) env->GetIntArrayRegion(algorithms, 0, count, values.data());
    for (jint value : values) request.algorithms.push_back(static_cast<LONG>(value));
  }
  request.client_data_json = ByteVector(env, clientDataJson);
  request.timeout_ms = static_cast<DWORD>(timeoutMs);
  request.exclude = Descriptors(env, excludeIds, excludeTransports);
  request.attachment = static_cast<DWORD>(attachment);
  request.resident_key = static_cast<int>(residentKey);
  request.user_verification = static_cast<DWORD>(userVerification);
  request.attestation = static_cast<DWORD>(attestation);
  GUID guid{};
  bool cancellable = Cancellation(env, cancellation, &guid);

  HWND owner = webauthn::ForegroundWindowOf(static_cast<DWORD>(pid));
  if (owner == nullptr) {
    SetInt(env, info, 0, 1);
    ALOGW("webauthn: pid %d does not own the foreground window; no credential dialog",
          static_cast<int>(pid));
    return static_cast<jint>(webauthn::kNoOwner);
  }
  webauthn::Attestation result;
  HRESULT hr = webauthn::MakeCredential(owner, request, cancellable ? &guid : nullptr, &result);
  if (hr != S_OK) {
    ALOGW("webauthn: MakeCredential for pid %d: HRESULT 0x%08lX", static_cast<int>(pid),
          static_cast<unsigned long>(hr));
    SetErrorName(env, out, 3, hr);
    return static_cast<jint>(hr);
  }
  SetBytes(env, out, 0, result.credential_id);
  SetBytes(env, out, 1, result.authenticator_data);
  SetBytes(env, out, 2, result.attestation_object);
  SetInt(env, info, 1, static_cast<jint>(result.used_transport));
  SetInt(env, info, 2, static_cast<jint>(result.transports));
  SetInt(env, info, 3, static_cast<jint>(result.resident_key));
  return static_cast<jint>(S_OK);
}

// info: [0] 1 when the caller does not own the foreground window, [1] the used
// transport. out: [0] credential id, [1] authenticator data, [2] signature,
// [3] user id, [4] Windows' error name.
jint WindowsWebAuthn_getAssertion(JNIEnv* env, jclass, jint pid, jstring rpId,
                                  jbyteArray clientDataJson, jint timeoutMs,
                                  jobjectArray allowIds, jintArray allowTransports,
                                  jint userVerification, jbyteArray cancellation,
                                  jobjectArray out, jintArray info) {
  webauthn::GetAssertionRequest request;
  request.rp_id = WideString(env, rpId);
  request.client_data_json = ByteVector(env, clientDataJson);
  request.timeout_ms = static_cast<DWORD>(timeoutMs);
  request.allow = Descriptors(env, allowIds, allowTransports);
  request.user_verification = static_cast<DWORD>(userVerification);
  GUID guid{};
  bool cancellable = Cancellation(env, cancellation, &guid);

  HWND owner = webauthn::ForegroundWindowOf(static_cast<DWORD>(pid));
  if (owner == nullptr) {
    SetInt(env, info, 0, 1);
    ALOGW("webauthn: pid %d does not own the foreground window; no credential dialog",
          static_cast<int>(pid));
    return static_cast<jint>(webauthn::kNoOwner);
  }
  webauthn::Assertion result;
  HRESULT hr = webauthn::GetAssertion(owner, request, cancellable ? &guid : nullptr, &result);
  if (hr != S_OK) {
    ALOGW("webauthn: GetAssertion for pid %d: HRESULT 0x%08lX", static_cast<int>(pid),
          static_cast<unsigned long>(hr));
    SetErrorName(env, out, 4, hr);
    return static_cast<jint>(hr);
  }
  SetBytes(env, out, 0, result.credential_id);
  SetBytes(env, out, 1, result.authenticator_data);
  SetBytes(env, out, 2, result.signature);
  SetBytes(env, out, 3, result.user_id);
  SetInt(env, info, 1, static_cast<jint>(result.used_transport));
  return static_cast<jint>(S_OK);
}

const JNINativeMethod gMethods[] = {
    {"nativeApiVersion", "()I", GESHER_JNI(WindowsWebAuthn_apiVersion)},
    {"nativeNewCancellation", "()[B", GESHER_JNI(WindowsWebAuthn_newCancellation)},
    {"nativeCancel", "([B)I", GESHER_JNI(WindowsWebAuthn_cancel)},
    {"nativeMakeCredential",
     "(ILjava/lang/String;Ljava/lang/String;[BLjava/lang/String;Ljava/lang/String;[I[BI[[B[I"
     "IIII[B[Ljava/lang/Object;[I)I",
     GESHER_JNI(WindowsWebAuthn_makeCredential)},
    {"nativeGetAssertion", "(ILjava/lang/String;[BI[[B[II[B[Ljava/lang/Object;[I)I",
     GESHER_JNI(WindowsWebAuthn_getAssertion)},
};

}  // namespace

int register_gesher_webauthn(JNIEnv* env) {
  return RegisterGesherNatives(env, "com/gesher/server/WindowsWebAuthn", gMethods,
                               NELEM(gMethods));
}

}  // namespace android
