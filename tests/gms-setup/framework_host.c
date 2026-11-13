/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Local native identity/allocation boundaries only; no Binder transport or storage. */
#include "../component-caller/framework_host.c"
#include <stdint.h>
#include <stdlib.h>
JNIEXPORT jlong JNICALL Java_android_os_Binder_getNativeFinalizer(JNIEnv* env, jclass cls) { return (jlong)(uintptr_t)free; }
JNIEXPORT jlong JNICALL Java_android_os_Binder_getNativeBBinderHolder(JNIEnv* env, jclass cls) { return (jlong)(uintptr_t)calloc(1, 1); }
JNIEXPORT void JNICALL Java_libcore_util_NativeAllocationRegistry_applyFreeFunction(JNIEnv* env, jclass cls, jlong function, jlong pointer) {
    if (function == (jlong)(uintptr_t)free) free((void*)(uintptr_t)pointer);
}
JNIEXPORT jint JNICALL Java_dalvik_system_VMRuntime_getNotifyNativeInterval(JNIEnv* env, jclass cls) { return 1024; }
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_notifyNativeAllocationsInternal(JNIEnv* env, jobject self) { }
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_registerNativeAllocation(JNIEnv* env, jobject self, jlong size) { }
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_registerNativeFree(JNIEnv* env, jobject self, jlong size) { }
