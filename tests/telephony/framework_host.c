/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Host-only framework natives for the telephony fixture: silent logging, the pinned
 * Android 16 version properties and JDK-backed arrays. Portable C11 (Linux and Windows
 * JDKs); it does not model Binder, Parcel or system properties writes. */
#include <jni.h>
#include <string.h>
#include <time.h>

JNIEXPORT jboolean JNICALL Java_android_util_Log_isLoggable(JNIEnv* env, jclass cls, jstring tag, jint priority) {
    return JNI_FALSE;
}
JNIEXPORT jint JNICALL Java_android_util_Log_println_1native(JNIEnv* env, jclass cls, jint buf, jint priority,
        jstring tag, jstring msg) {
    return 0;
}
JNIEXPORT jint JNICALL Java_android_util_Log_logger_1entry_1max_1payload_1native(JNIEnv* env, jclass cls) {
    return 4068;
}
JNIEXPORT jobject JNICALL Java_dalvik_system_VMRuntime_newUnpaddedArray(JNIEnv* env, jobject runtime,
        jclass component, jint length) {
    jclass array = (*env)->FindClass(env, "java/lang/reflect/Array");
    jmethodID create = (*env)->GetStaticMethodID(env, array, "newInstance", "(Ljava/lang/Class;I)Ljava/lang/Object;");
    return (*env)->CallStaticObjectMethod(env, array, create, component, length);
}
JNIEXPORT jboolean JNICALL Java_dalvik_system_VMRuntime_is64Bit(JNIEnv* env, jobject runtime) {
    return JNI_TRUE;
}
static jlong now_millis(void) {
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (jlong) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
JNIEXPORT jlong JNICALL Java_android_os_SystemClock_uptimeMillis(JNIEnv* env, jclass cls) { return now_millis(); }
JNIEXPORT jlong JNICALL Java_android_os_SystemClock_elapsedRealtime(JNIEnv* env, jclass cls) { return now_millis(); }
JNIEXPORT jstring JNICALL Java_android_os_SystemProperties_native_1get__Ljava_lang_String_2Ljava_lang_String_2(
        JNIEnv* env, jclass cls, jstring key, jstring fallback) {
    const char* name = (*env)->GetStringUTFChars(env, key, 0);
    const char* value = 0;
    if (!strcmp(name, "ro.build.version.codename") || !strcmp(name, "ro.build.version.all_codenames")) value = "REL";
    else if (!strcmp(name, "ro.build.version.sdk_full")) value = "36.0";
    (*env)->ReleaseStringUTFChars(env, key, name);
    return value ? (*env)->NewStringUTF(env, value) : fallback;
}
JNIEXPORT jint JNICALL Java_android_os_SystemProperties_native_1get_1int__Ljava_lang_String_2I(
        JNIEnv* env, jclass cls, jstring key, jint fallback) {
    const char* name = (*env)->GetStringUTFChars(env, key, 0);
    jint value = !strcmp(name, "ro.build.version.sdk") ? 36 : fallback;
    (*env)->ReleaseStringUTFChars(env, key, name);
    return value;
}
JNIEXPORT jlong JNICALL Java_android_os_SystemProperties_native_1get_1long__Ljava_lang_String_2J(
        JNIEnv* env, jclass cls, jstring key, jlong fallback) {
    return fallback;
}
JNIEXPORT jboolean JNICALL Java_android_os_SystemProperties_native_1get_1boolean__Ljava_lang_String_2Z(
        JNIEnv* env, jclass cls, jstring key, jboolean fallback) {
    return fallback;
}
JNIEXPORT jlong JNICALL Java_android_os_SystemProperties_native_1find(JNIEnv* env, jclass cls, jstring key) {
    return 0;
}
/* Process identity for framework caches (PropertyInvalidatedCache); not a Binder caller. */
JNIEXPORT jint JNICALL Java_libcore_io_Linux_nativeGetuid(JNIEnv* env, jclass cls) { return 1000; }
JNIEXPORT jint JNICALL Java_libcore_io_Linux_nativeGetpid(JNIEnv* env, jclass cls) { return 1; }
