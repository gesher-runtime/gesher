/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Existing clock/identity/initialization boundaries; no native window emulation. */
#include "../location/framework_host.c"

static void observe(JNIEnv* env, jlong pointer, jint operation) {
    jclass probe = (*env)->FindClass(env, "ActivityWindowActivationHost");
    if (probe == NULL) return;
    jmethodID record = (*env)->GetStaticMethodID(env, probe, "recordNative", "(JI)V");
    if (record != NULL) (*env)->CallStaticVoidMethod(env, probe, record, pointer, operation);
    (*env)->DeleteLocalRef(env, probe);
}

JNIEXPORT void JNICALL Java_com_gesher_app_window_NativeWindow_nShow(
        JNIEnv* env, jclass type, jlong pointer) {
    (void)type;
    observe(env, pointer, 1);
}

JNIEXPORT void JNICALL Java_com_gesher_app_window_NativeWindow_nActivate(
        JNIEnv* env, jclass type, jlong pointer) {
    (void)type;
    observe(env, pointer, 0);
}

JNIEXPORT void JNICALL Java_com_gesher_app_window_NativeWindow_nRestore(
        JNIEnv* env, jclass type, jlong pointer) {
    (void)type;
    observe(env, pointer, 2);
}
