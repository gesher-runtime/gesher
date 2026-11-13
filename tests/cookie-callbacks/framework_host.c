/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Real framework Handler/Looper/MessageQueue over a pthread wait/wake boundary.
 * No ART/native Windows/WebView2 claim. Identity and tracing are host diagnostics. */
#include "../component-caller/framework_host.c"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
struct queue { pthread_mutex_t lock; pthread_cond_t wake; int pending; };
JNIEXPORT jlong JNICALL Java_android_os_MessageQueue_nativeInit(JNIEnv* e, jclass c) {
    struct queue* q=calloc(1,sizeof(*q));
    if (!q) { (*e)->ThrowNew(e,(*e)->FindClass(e,"java/lang/OutOfMemoryError"),"host queue"); return 0; }
    pthread_mutex_init(&q->lock,0); pthread_cond_init(&q->wake,0); return (jlong)(uintptr_t)q;
}
JNIEXPORT void JNICALL Java_android_os_MessageQueue_nativeDestroy(JNIEnv* e,jclass c,jlong p) {
    struct queue* q=(void*)(uintptr_t)p;
    pthread_cond_destroy(&q->wake); pthread_mutex_destroy(&q->lock); free(q);
}
JNIEXPORT void JNICALL Java_android_os_MessageQueue_nativePollOnce(JNIEnv* e,jobject o,jlong p,jint ms) {
    struct queue* q=(void*)(uintptr_t)p;
    pthread_mutex_lock(&q->lock);
    if (!q->pending && ms!=0) {
        if (ms<0) { while(!q->pending) pthread_cond_wait(&q->wake,&q->lock); }
        else {
            struct timespec t;clock_gettime(CLOCK_REALTIME,&t);
            t.tv_sec+=ms/1000;t.tv_nsec+=(long)(ms%1000)*1000000;
            if(t.tv_nsec>=1000000000){t.tv_sec++;t.tv_nsec-=1000000000;}
            while(!q->pending && pthread_cond_timedwait(&q->wake,&q->lock,&t)==0) {}
        }
    }
    q->pending=0;pthread_mutex_unlock(&q->lock);
}
JNIEXPORT void JNICALL Java_android_os_MessageQueue_nativeWake(JNIEnv* e,jclass c,jlong p) {
    struct queue* q=(void*)(uintptr_t)p;pthread_mutex_lock(&q->lock);
    q->pending=1;pthread_cond_signal(&q->wake);pthread_mutex_unlock(&q->lock);
}
JNIEXPORT void JNICALL Java_android_os_MessageQueue_nativeSetSkipEpollWaitForZeroTimeout(JNIEnv* e,jclass c,jlong p) {}
JNIEXPORT jint JNICALL Java_libcore_io_Linux_nativeGettid(JNIEnv* e,jclass c) {return getpid();}
JNIEXPORT void JNICALL Java_android_os_Process_setThreadPriorityNative(JNIEnv* e,jclass c,jint tid,jint p) {}
JNIEXPORT jint JNICALL Java_android_os_Process_getProcessGroup(JNIEnv* e,jclass c,jint p) {return 0;}
JNIEXPORT void JNICALL Java_android_os_Binder_flushPendingCommands(JNIEnv* e,jclass c) {}
JNIEXPORT jint JNICALL Java_android_os_Binder_getCallingWorkSourceUid(JNIEnv* e,jclass c) {return -1;}
