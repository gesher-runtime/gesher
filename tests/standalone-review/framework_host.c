/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Existing host clock/Binder boundaries. Only our own host process has a live
 * generation here; actual Windows process identity remains a runtime test. */
#include "../activity-finish/framework_host.c"
JNIEXPORT jlong JNICALL Java_com_gesher_server_Zygote_nativeStartTime(
        JNIEnv* env, jclass type, jint pid) {
    (void)env; (void)type;
    return pid == getpid() ? 1 : 0;
}
