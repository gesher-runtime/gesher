/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Existing host framework initialization, clock and Binder identity boundaries; no policy replacement. */
#include "../appops/framework_host.c"

/* Tracing is disabled in the host fixture. */
JNIEXPORT jboolean JNICALL Java_android_os_Trace_nativeIsTagEnabled(JNIEnv* env, jclass type, jlong tag) {
    (void)env;
    (void)type;
    (void)tag;
    return JNI_FALSE;
}
