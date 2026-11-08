/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Test-only JVM initialization/native boundaries, not a Windows implementation. */
#include "../notification/framework_host.c"
#include <pthread.h>
static pthread_mutex_t request_mutex = PTHREAD_MUTEX_INITIALIZER;
static int request_count;
static jboolean fail_next, last_system, last_display;
static jstring last_reason;
JNIEXPORT jint JNICALL Java_com_gesher_server_WindowsHost_nativeClipboardSequence(JNIEnv* env,jclass cls) { return 0; }
JNIEXPORT jboolean JNICALL Java_com_gesher_server_WindowsHost_nativeKeepAwake(JNIEnv* env,jclass cls,jstring reason,jboolean system,jboolean display) {
    pthread_mutex_lock(&request_mutex);
    request_count++; last_system=system; last_display=display;
    if(last_reason) (*env)->DeleteGlobalRef(env,last_reason);
    last_reason=(*env)->NewGlobalRef(env,reason);
    jboolean result=!fail_next;fail_next=JNI_FALSE;
    pthread_mutex_unlock(&request_mutex);return result;
}
JNIEXPORT void JNICALL Java_PowerWakeLockHost_reset(JNIEnv* env,jclass cls) {
    pthread_mutex_lock(&request_mutex);
    request_count=0;fail_next=last_system=last_display=JNI_FALSE;
    if(last_reason) (*env)->DeleteGlobalRef(env,last_reason);last_reason=0;
    pthread_mutex_unlock(&request_mutex);
}
JNIEXPORT void JNICALL Java_PowerWakeLockHost_failNext(JNIEnv* env,jclass cls) { pthread_mutex_lock(&request_mutex);fail_next=JNI_TRUE;pthread_mutex_unlock(&request_mutex); }
JNIEXPORT jint JNICALL Java_PowerWakeLockHost_calls(JNIEnv* env,jclass cls) { pthread_mutex_lock(&request_mutex);jint value=request_count;pthread_mutex_unlock(&request_mutex);return value; }
JNIEXPORT jboolean JNICALL Java_PowerWakeLockHost_system(JNIEnv* env,jclass cls) { pthread_mutex_lock(&request_mutex);jboolean value=last_system;pthread_mutex_unlock(&request_mutex);return value; }
JNIEXPORT jboolean JNICALL Java_PowerWakeLockHost_display(JNIEnv* env,jclass cls) { pthread_mutex_lock(&request_mutex);jboolean value=last_display;pthread_mutex_unlock(&request_mutex);return value; }
JNIEXPORT jstring JNICALL Java_PowerWakeLockHost_reason(JNIEnv* env,jclass cls) { pthread_mutex_lock(&request_mutex);jstring value=(*env)->NewLocalRef(env,last_reason);pthread_mutex_unlock(&request_mutex);return value; }
