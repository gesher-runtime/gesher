/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Android initialization plus observed geometry boundaries; no Win32 emulation. */
#include "../location/framework_host.c"

static jclass probe(JNIEnv* env) {
    return (*env)->FindClass(env, "WindowParentPlacementHost");
}

JNIEXPORT jintArray JNICALL Java_com_gesher_app_window_NativeWindow_nGetScreenBounds(
        JNIEnv* env, jclass type, jlong pointer) {
    (void)type;
    jclass test = probe(env);
    if (test == NULL) return NULL;
    jmethodID method = (*env)->GetStaticMethodID(env, test, "screenBounds", "(J)[I");
    jintArray result = method == NULL ? NULL :
            (jintArray)(*env)->CallStaticObjectMethod(env, test, method, pointer);
    (*env)->DeleteLocalRef(env, test);
    return result;
}

JNIEXPORT void JNICALL Java_com_gesher_app_window_NativeWindow_nSetScreenPosition(
        JNIEnv* env, jclass type, jlong pointer, jint x, jint y) {
    (void)type;
    jclass test = probe(env);
    if (test == NULL) return;
    jmethodID method = (*env)->GetStaticMethodID(env, test, "position", "(JII)V");
    if (method != NULL) (*env)->CallStaticVoidMethod(env, test, method, pointer, x, y);
    (*env)->DeleteLocalRef(env, test);
}

JNIEXPORT void JNICALL Java_com_gesher_app_window_NativeWindow_nSetClientSize(
        JNIEnv* env, jclass type, jlong pointer, jint width, jint height) {
    (void)type;
    jclass test = probe(env);
    if (test == NULL) return;
    jmethodID method = (*env)->GetStaticMethodID(env, test, "size", "(JII)V");
    if (method != NULL) (*env)->CallStaticVoidMethod(env, test, method, pointer, width, height);
    (*env)->DeleteLocalRef(env, test);
}

JNIEXPORT jint JNICALL Java_com_gesher_app_window_NativeWindow_nGetWidth(
        JNIEnv* env, jclass type, jlong pointer) {
    jintArray bounds = Java_com_gesher_app_window_NativeWindow_nGetScreenBounds(env, type, pointer);
    if (bounds == NULL) return 0;
    jint values[4];
    (*env)->GetIntArrayRegion(env, bounds, 0, 4, values);
    (*env)->DeleteLocalRef(env, bounds);
    return values[2] - values[0];
}

JNIEXPORT jint JNICALL Java_com_gesher_app_window_NativeWindow_nGetHeight(
        JNIEnv* env, jclass type, jlong pointer) {
    jintArray bounds = Java_com_gesher_app_window_NativeWindow_nGetScreenBounds(env, type, pointer);
    if (bounds == NULL) return 0;
    jint values[4];
    (*env)->GetIntArrayRegion(env, bounds, 0, 4, values);
    (*env)->DeleteLocalRef(env, bounds);
    return values[3] - values[1];
}

JNIEXPORT void JNICALL Java_com_gesher_app_window_NativeWindow_nSetPopupBounds(
        JNIEnv* env, jclass type, jlong pointer, jlong anchor, jint x, jint y,
        jint width, jint height) {
    (void)type;
    jclass test = probe(env);
    if (test == NULL) return;
    jmethodID method = (*env)->GetStaticMethodID(env, test, "popup", "(JJIIII)V");
    if (method != NULL) (*env)->CallStaticVoidMethod(env, test, method,
            pointer, anchor, x, y, width, height);
    (*env)->DeleteLocalRef(env, test);
}
