/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Existing clock/logging/Binder-holder initialization boundaries; no Parcel substitute. */
#ifdef _WIN32
#include <errno.h>
#include <sys/types.h>
/* Notification FD reads are outside this host connectivity fixture. */
static ssize_t unsupported_pread(int fd, void* data, size_t count, off_t offset) {
    errno = ENOSYS;
    return -1;
}
#define pread unsupported_pread
#endif
#include "../location/framework_host.c"

/* Tracing is disabled at this host boundary; Handler/MessageQueue Java removal
 * is real and needs no native poller. */
JNIEXPORT jlong JNICALL Java_android_os_PerfettoTrace_00024Category_native_1delete(JNIEnv* e, jclass c) { return (jlong)(uintptr_t)free; }
JNIEXPORT jlong JNICALL Java_android_os_PerfettoTrace_00024Category_native_1init(JNIEnv* e, jclass c, jstring n, jstring t, jstring s) { return (jlong)(uintptr_t)calloc(1, 1); }
JNIEXPORT jlong JNICALL Java_android_os_PerfettoTrace_00024Category_native_1get_1extra_1ptr(JNIEnv* e, jclass c, jlong p) { return 0; }
JNIEXPORT void JNICALL Java_android_os_PerfettoTrace_00024Category_native_1register(JNIEnv* e, jclass c, jlong p) { }
JNIEXPORT jboolean JNICALL Java_android_os_PerfettoTrace_00024Category_native_1is_1enabled(JNIEnv* e, jclass c, jlong p) { return JNI_FALSE; }

/* The production service reads this declared network snapshot boundary. The
 * deterministic host fixture supplies successive adapter snapshots; no Windows
 * discovery, change notification, or interface implementation is substituted. */
JNIEXPORT jstring JNICALL Java_com_gesher_net_WindowsNetworks_nativeSnapshot(JNIEnv* env, jclass type) {
    jclass fixture = (*env)->FindClass(env, "ConnectivityServiceSnapshotHost");
    if (!fixture) return NULL;
    jmethodID read = (*env)->GetStaticMethodID(env, fixture, "readSnapshot", "()Ljava/lang/String;");
    if (!read) return NULL;
    return (jstring)(*env)->CallStaticObjectMethod(env, fixture, read);
}
