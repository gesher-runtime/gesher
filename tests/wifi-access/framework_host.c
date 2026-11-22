/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Native initialization and monotonic clocks only. Radio data is supplied through
 * an explicit local Binder fixture; production managers never use a fake backend. */
#include "../connectivity-callback/framework_host.c"

JNIEXPORT jlong JNICALL Java_android_os_SystemClock_uptimeNanos(JNIEnv* env, jclass cls) {
    return Java_android_os_SystemClock_elapsedRealtimeNanos(env, cls);
}
