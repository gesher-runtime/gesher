/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Host Binder/clock/allocation boundaries; no ART image or dex2oat process in this JVM. */
#include "../location/framework_host.c"
JNIEXPORT jstring JNICALL Java_com_gesher_server_Zygote_nativeCommandLine(JNIEnv* env, jclass type) {
    return (*env)->NewStringUTF(env, "installer-identity-host -Xbootclasspath:host-fixture");
}
