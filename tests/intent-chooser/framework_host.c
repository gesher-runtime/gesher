/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
/* Host identity/initialization, PendingIntent parcel-size and tracing boundaries only. */
#include "../pending-intent/framework_host.c"
JNIEXPORT jlong JNICALL Java_android_os_SystemClock_elapsedRealtime(JNIEnv* env, jclass type) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (jlong)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
JNIEXPORT jlong JNICALL Java_android_os_SystemClock_elapsedRealtimeNanos(JNIEnv* env, jclass type) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (jlong)now.tv_sec * 1000000000 + now.tv_nsec;
}
/* Android tracing has no host session; leave the real provider transport unmodified. */
JNIEXPORT jboolean JNICALL Java_android_os_Trace_nativeIsTagEnabled(JNIEnv* env, jclass type, jlong tag) {
    return JNI_FALSE;
}
