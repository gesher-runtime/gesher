/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#include <jni.h>

/* The only host boundary: the Windows VM executable is on D:, as in CI. */
JNIEXPORT jstring JNICALL Java_com_gesher_server_Zygote_nativeExePath(JNIEnv* env, jclass type) {
    (void)type;
    return (*env)->NewStringUTF(env, "D:\\gesher\\dalvikvm.exe");
}
