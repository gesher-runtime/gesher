/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Host clock and recorded process boundaries: never invoke OS process control. */
#include "../pending-intent/framework_host.c"

JNIEXPORT jlong JNICALL Java_android_os_SystemClock_elapsedRealtime(JNIEnv* env, jclass type) {
    return Java_android_os_SystemClock_uptimeMillis(env, type);
}

static jclass fixture(JNIEnv* env) {
    jclass thread = (*env)->FindClass(env, "java/lang/Thread");
    jobject current = (*env)->CallStaticObjectMethod(env, thread,
            (*env)->GetStaticMethodID(env, thread, "currentThread", "()Ljava/lang/Thread;"));
    jobject loader = (*env)->CallObjectMethod(env, current,
            (*env)->GetMethodID(env, thread, "getContextClassLoader", "()Ljava/lang/ClassLoader;"));
    jclass loader_type = (*env)->FindClass(env, "java/lang/ClassLoader");
    jstring name = (*env)->NewStringUTF(env, "com.gesher.server.PackageEnabledLifecycleFixture");
    return (jclass)(*env)->CallObjectMethod(env, loader,
            (*env)->GetMethodID(env, loader_type, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), name);
}
JNIEXPORT jlong JNICALL Java_com_gesher_server_Zygote_nativeChildStartTime(JNIEnv* env, jclass type, jint pid) {
    jclass target = fixture(env);
    return (*env)->CallStaticLongMethod(env, target,
            (*env)->GetStaticMethodID(env, target, "childStartTime", "(I)J"), pid);
}
JNIEXPORT jboolean JNICALL Java_com_gesher_server_Zygote_nativeKillChild(JNIEnv* env, jclass type, jint pid, jlong time) {
    jclass target = fixture(env);
    return (*env)->CallStaticBooleanMethod(env, target,
            (*env)->GetStaticMethodID(env, target, "killChild", "(IJ)Z"), pid, time);
}
JNIEXPORT jint JNICALL Java_com_gesher_server_Zygote_nativeExitCode(JNIEnv* env, jclass type, jint pid) {
    jclass target = fixture(env);
    return (*env)->CallStaticIntMethod(env, target,
            (*env)->GetStaticMethodID(env, target, "exitCode", "(I)I"), pid);
}
JNIEXPORT jboolean JNICALL Java_com_gesher_server_Zygote_nativeKill(JNIEnv* env, jclass type, jint pid) {
    (*env)->ThrowNew(env, (*env)->FindClass(env, "java/lang/AssertionError"), "Lifecycle attempted bare PID termination");
    return JNI_FALSE;
}
