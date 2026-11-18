/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Existing host identity/log/clock boundaries only; no UI or UIA emulation. */
#ifdef _WIN32
#include <errno.h>
#include <sys/types.h>
static ssize_t uia_unsupported_pread(int fd, void* data, size_t count, off_t offset) {
    errno = ENOSYS;
    return -1;
}
#define pread uia_unsupported_pread
#endif
#include "../notification/framework_host.c"

/* Observe the actual bridge event output without Windows UI Automation. */
JNIEXPORT void JNICALL Java_com_gesher_app_accessibility_UiaBridge_nativeRaise(
        JNIEnv* env, jclass cls, jlong hwnd, jlong id, jint kind, jstring text) {
    (void)cls; (void)hwnd; (void)kind; (void)text;
    jclass probe = (*env)->FindClass(env, "UiaEventIdentityHost");
    if (probe == NULL) return;
    jmethodID record = (*env)->GetStaticMethodID(env, probe, "record", "(J)V");
    if (record != NULL) (*env)->CallStaticVoidMethod(env, probe, record, id);
    (*env)->DeleteLocalRef(env, probe);
}
