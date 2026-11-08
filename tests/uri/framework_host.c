/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Native host boundaries only. Keep production Binder PID/UID authentication in Java. */
#define Java_android_os_Binder_getCallingPid uri_original_getCallingPid
#define Java_android_os_Binder_getCallingUid uri_original_getCallingUid
#define Java_android_os_Binder_clearCallingIdentity uri_original_clearCallingIdentity
#define Java_android_os_Binder_restoreCallingIdentity uri_original_restoreCallingIdentity
#include "../package/framework_host.c"
#undef Java_android_os_Binder_getCallingPid
#undef Java_android_os_Binder_getCallingUid
#undef Java_android_os_Binder_clearCallingIdentity
#undef Java_android_os_Binder_restoreCallingIdentity
static _Thread_local jlong identity;
JNIEXPORT jint JNICALL Java_android_os_Binder_getCallingPid(JNIEnv* env,jclass cls) {
    return identity ? (jint)identity : getpid();
}
JNIEXPORT jint JNICALL Java_android_os_Binder_getCallingUid(JNIEnv* env,jclass cls) {
    return identity ? (jint)((unsigned long long)identity >> 32) : 1000;
}
JNIEXPORT jlong JNICALL Java_android_os_Binder_clearCallingIdentity(JNIEnv* env,jclass cls) {
    jlong previous=identity;identity=0;return previous;
}
JNIEXPORT void JNICALL Java_android_os_Binder_restoreCallingIdentity(JNIEnv* env,jclass cls,jlong value) {
    identity=value;
}
