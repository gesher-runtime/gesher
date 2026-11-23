/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Host Binder/clock/allocation boundaries; this JVM has no ART image or dex2oat. */
#include "../location/framework_host.c"
JNIEXPORT jstring JNICALL Java_com_gesher_server_Zygote_nativeCommandLine(JNIEnv* env, jclass type) {
    return (*env)->NewStringUTF(env, "install-source-host -Xbootclasspath:host-fixture");
}
