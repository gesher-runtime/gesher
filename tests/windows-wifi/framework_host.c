/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
/* Explicit test-native boundary; production threading, parsing and cache code are unmodified. */
#include "../wifi-configuration/framework_host.c"
JNIEXPORT jlong JNICALL Java_android_os_SystemClock_elapsedRealtimeNanos(JNIEnv* env, jclass type) {
    struct timespec now;
    if (clock_gettime(CLOCK_BOOTTIME, &now) != 0) return 0;
    return (jlong) now.tv_sec * 1000000000 + now.tv_nsec;
}
static jstring query(JNIEnv* env, jclass type, jboolean scan, jobject eligible) {
    jclass fixture = (*env)->FindClass(env, "com/gesher/net/WindowsWifiFixture");
    jmethodID method = (*env)->GetStaticMethodID(env, fixture, "query", "(ZLjava/util/function/BooleanSupplier;)Ljava/lang/String;");
    return (jstring) (*env)->CallStaticObjectMethod(env, fixture, method, scan, eligible);
}
JNIEXPORT void JNICALL Java_com_gesher_net_WindowsWifiFixture_installNativeBoundary(JNIEnv* env, jclass type) {
    jclass wifi = (*env)->FindClass(env, "com/gesher/net/WindowsWifi");
    JNINativeMethod method = {"nativeQuery", "(ZLjava/util/function/BooleanSupplier;)Ljava/lang/String;", (void*) query};
    (*env)->RegisterNatives(env, wifi, &method, 1);
}
