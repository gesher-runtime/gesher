/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#include <jni.h>
#ifndef FIXTURE_VALUE
#define FIXTURE_VALUE 100
#endif
static jint constructors;
static jint onloads;
extern int android_get_application_target_sdk_version(void);
__attribute__((constructor)) static void initialize(void) { ++constructors; }
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)vm; (void)reserved;
  ++onloads;
  return JNI_VERSION_1_6;
}
JNIEXPORT jint JNICALL Java_gesher_nativepayload_Probe_value(JNIEnv* env, jclass type) {
  (void)env; (void)type;
  return FIXTURE_VALUE;
}
JNIEXPORT jint JNICALL Java_gesher_nativepayload_Probe_constructors(JNIEnv* env, jclass type) {
  (void)env; (void)type;
  return constructors;
}
JNIEXPORT jint JNICALL Java_gesher_nativepayload_Probe_onloads(JNIEnv* env, jclass type) {
  (void)env; (void)type;
  return onloads;
}
JNIEXPORT jint JNICALL Java_gesher_nativepayload_Probe_applicationTargetSdk(JNIEnv* env, jclass type) {
  (void)env; (void)type;
  return android_get_application_target_sdk_version();
}
