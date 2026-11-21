/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
/* Explicit host initialization boundary; never linked into production. */
#include "../component-caller/framework_host.c"
#include <time.h>
JNIEXPORT jlong JNICALL Java_android_os_SystemClock_elapsedRealtime(JNIEnv* env, jclass type) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (jlong)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
