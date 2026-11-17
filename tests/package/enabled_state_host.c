/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Initialization/identity boundaries and recording process termination; never kill a host PID. */
#include "../location/framework_host.c"
JNIEXPORT jlong JNICALL Java_com_gesher_server_Zygote_nativeChildStartTime(JNIEnv* env, jclass type, jint pid) {
    return 0; /* This host adapter has never spawned a native child. */
}
JNIEXPORT jboolean JNICALL Java_com_gesher_server_Zygote_nativeKillChild(JNIEnv* env, jclass type, jint pid, jlong time) {
    jclass fixture = (*env)->FindClass(env, "gesher/services/PackageEnabledStateContractTest");
    jmethodID observed = (*env)->GetStaticMethodID(env, fixture, "nativeKillObserved", "(IJ)Z");
    return (*env)->CallStaticBooleanMethod(env, fixture, observed, pid, time);
}
JNIEXPORT jboolean JNICALL Java_com_gesher_server_Zygote_nativeKill(JNIEnv* env, jclass type, jint pid) {
    jclass error = (*env)->FindClass(env, "java/lang/AssertionError");
    (*env)->ThrowNew(env, error, "Enabled-state retirement attempted a bare PID kill");
    return JNI_FALSE;
}
