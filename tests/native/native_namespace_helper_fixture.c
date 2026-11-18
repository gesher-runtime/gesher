/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#include <jni.h>

static JavaVM* onload_vm;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)reserved;
  onload_vm = vm;
  return JNI_VERSION_1_6;
}

// This fixture is linked against the official API 31+ libnativehelper stub.
// Its versioned import must resolve in the first application namespace,
// before another JNI library or a catalog query initializes the bridge.
JNIEXPORT jint JNICALL Java_gesher_nativepayload_Probe_helperVmIdentity(JNIEnv* env, jclass type) {
  (void)type;
  jint result = 0;
  jsize count = -1;
  JavaVM* found = NULL;
  if (JNI_GetCreatedJavaVMs(&found, 1, &count) == JNI_OK) result |= 1;
  if (count == 1 && found != NULL) result |= 2;
  if (found != NULL && found == onload_vm) result |= 4;
  JavaVM* from_env = NULL;
  if ((*env)->GetJavaVM(env, &from_env) == JNI_OK && from_env == found) result |= 8;
  void* current_env = NULL;
  if (found != NULL && (*found)->GetEnv(found, &current_env, JNI_VERSION_1_6) == JNI_OK &&
      current_env == env) result |= 16;
  return result;
}
