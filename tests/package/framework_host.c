/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Reuse the existing, explicitly host-only native boundaries. Package metadata additionally
 * initializes Build.VERSION; give that boundary the same pinned Android 16 version. */
#define Java_android_os_SystemProperties_native_1get__Ljava_lang_String_2Ljava_lang_String_2 package_host_string_property
#include "../notification/framework_host.c"
#undef Java_android_os_SystemProperties_native_1get__Ljava_lang_String_2Ljava_lang_String_2
JNIEXPORT jstring JNICALL Java_android_os_SystemProperties_native_1get__Ljava_lang_String_2Ljava_lang_String_2(
        JNIEnv* env, jclass cls, jstring key, jstring fallback) {
    const char* name = (*env)->GetStringUTFChars(env, key, 0);
    int full_version = !strcmp(name, "ro.build.version.sdk_full");
    (*env)->ReleaseStringUTFChars(env, key, name);
    return full_version ? (*env)->NewStringUTF(env, "36.0")
            : package_host_string_property(env, cls, key, fallback);
}
