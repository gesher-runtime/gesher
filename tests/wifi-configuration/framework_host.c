/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Native initialization/logging and the real Linux elapsed clock only; no Parcel adapter. */
#include "../component-caller/framework_host.c"
#include <time.h>

JNIEXPORT jlong JNICALL Java_android_os_SystemClock_elapsedRealtime(JNIEnv* env, jclass type) {
    struct timespec now;
    if (clock_gettime(CLOCK_BOOTTIME, &now) != 0) {
        (*env)->ThrowNew(env, (*env)->FindClass(env, "java/lang/IllegalStateException"),
                        "CLOCK_BOOTTIME read failed");
        return 0;
    }
    return (jlong)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
