/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Host initialization/identity only. No permission or camera decision is replaced. */
#define Java_android_os_Binder_getCallingPid camera_original_getCallingPid
#define Java_android_os_Binder_getCallingUid camera_original_getCallingUid
#define Java_android_os_Binder_clearCallingIdentity camera_original_clearCallingIdentity
#define Java_android_os_Binder_restoreCallingIdentity camera_original_restoreCallingIdentity
#include "../package/framework_host.c"
#undef Java_android_os_Binder_getCallingPid
#undef Java_android_os_Binder_getCallingUid
#undef Java_android_os_Binder_clearCallingIdentity
#undef Java_android_os_Binder_restoreCallingIdentity
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
static _Thread_local jlong identity;
JNIEXPORT jlong JNICALL Java_android_os_SystemClock_elapsedRealtime(JNIEnv* env, jclass type) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (jlong)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
JNIEXPORT jint JNICALL Java_android_os_Binder_getCallingPid(JNIEnv* env, jclass type) {
    return identity ? (jint)identity : getpid();
}
JNIEXPORT jint JNICALL Java_android_os_Binder_getCallingUid(JNIEnv* env, jclass type) {
    return identity ? (jint)((unsigned long long)identity >> 32) : 10000;
}
JNIEXPORT jlong JNICALL Java_android_os_Binder_clearCallingIdentity(JNIEnv* env, jclass type) {
    jlong previous = identity;
    identity = 0;
    return previous;
}
JNIEXPORT void JNICALL Java_android_os_Binder_restoreCallingIdentity(JNIEnv* env, jclass type, jlong value) {
    identity = value;
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
