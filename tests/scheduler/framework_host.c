/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Reuse declared local identity/allocation/Parcel-write boundaries. Scheduler
 * dispatch is manual; this host clock does not test Windows suspend behavior. */
#include "../pending-intent/framework_host.c"
JNIEXPORT jlong JNICALL Java_android_os_SystemClock_elapsedRealtime(JNIEnv* env, jclass cls) {
    return Java_android_os_SystemClock_uptimeMillis(env, cls);
}
