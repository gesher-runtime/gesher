/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Real framework/service Java; only native initialization, transport identity,
 * clocks and Intent byte accounting are host boundaries. No Parcel reads. */
#define Java_libcore_io_Linux_nativeGetuid wifi_original_getuid
#include "../pending-intent/framework_host.c"
#undef Java_libcore_io_Linux_nativeGetuid

static jint own_uid = 1000;
static jint calling_uid(JNIEnv* env, jclass type) {
    return identity ? (jint)((unsigned long long)identity >> 32) : own_uid;
}
JNIEXPORT jint JNICALL Java_libcore_io_Linux_nativeGetuid(JNIEnv* env, jclass type) {
    return own_uid;
}
JNIEXPORT void JNICALL Java_com_gesher_server_WifiServiceHost_setOwnUid(JNIEnv* env, jclass type, jint uid) {
    own_uid = uid;
    jclass binder = (*env)->FindClass(env, "android/os/Binder");
    JNINativeMethod methods[] = {{"getCallingUid", "()I", (void*)calling_uid}};
    (*env)->RegisterNatives(env, binder, methods, 1);
}
JNIEXPORT jlong JNICALL Java_android_os_SystemClock_elapsedRealtime(JNIEnv* env, jclass type) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (jlong)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
JNIEXPORT jlong JNICALL Java_android_os_SystemClock_elapsedRealtimeNanos(JNIEnv* env, jclass type) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (jlong)now.tv_sec * 1000000000 + now.tv_nsec;
}
