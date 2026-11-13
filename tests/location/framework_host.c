/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Host initialization, clock and Binder-holder boundaries only. No Parcel transport. */
#include "../component-caller/framework_host.c"
#include <stdint.h>
#include <stdlib.h>

JNIEXPORT jlong JNICALL Java_android_os_SystemClock_elapsedRealtime(JNIEnv* env, jclass type) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (jlong)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
JNIEXPORT jlong JNICALL Java_android_os_SystemClock_elapsedRealtimeNanos(JNIEnv* env, jclass type) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (jlong)now.tv_sec * 1000000000 + now.tv_nsec;
}
JNIEXPORT jlong JNICALL Java_android_os_Binder_getNativeFinalizer(JNIEnv* env, jclass type) { return (jlong)(uintptr_t)free; }
JNIEXPORT jlong JNICALL Java_android_os_Binder_getNativeBBinderHolder(JNIEnv* env, jclass type) { return (jlong)(uintptr_t)calloc(1, 1); }
JNIEXPORT void JNICALL Java_libcore_util_NativeAllocationRegistry_applyFreeFunction(JNIEnv* env, jclass type, jlong function, jlong pointer) {
    if (function == (jlong)(uintptr_t)free) free((void*)(uintptr_t)pointer);
}
JNIEXPORT jint JNICALL Java_dalvik_system_VMRuntime_getNotifyNativeInterval(JNIEnv* env, jclass type) { return 1024; }
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_notifyNativeAllocationsInternal(JNIEnv* env, jobject self) { }
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_registerNativeAllocation(JNIEnv* env, jobject self, jlong size) { }
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_registerNativeFree(JNIEnv* env, jobject self, jlong size) { }
