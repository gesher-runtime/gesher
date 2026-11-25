/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Host identity/clock boundaries; no ART image or dex2oat process in this JVM. */
#include "../location/framework_host.c"
JNIEXPORT jstring JNICALL Java_com_gesher_server_Zygote_nativeCommandLine(JNIEnv* env, jclass type) {
    return (*env)->NewStringUTF(env, "blob-identity-host -Xbootclasspath:host-fixture");
}
