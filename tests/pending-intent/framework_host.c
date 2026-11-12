/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Host-only native initialization, identity and bounded-Intent Parcel write accounting.
 * The framework Intent, Bundle, PendingIntent, Binder, RemoteCallbackList and all
 * production services are unmodified. Native Parcel reads/transactions are deliberately
 * absent: the Windows fixture uses the real natives for generated AIDL transport. */
#include "../component-caller/framework_host.c"
#include <stdint.h>
#include <stdlib.h>

JNIEXPORT jlong JNICALL Java_android_os_Binder_getNativeFinalizer(JNIEnv* env, jclass cls) { return (jlong)(uintptr_t)free; }
JNIEXPORT jlong JNICALL Java_android_os_Binder_getNativeBBinderHolder(JNIEnv* env, jclass cls) { return (jlong)(uintptr_t)calloc(1, 1); }
JNIEXPORT void JNICALL Java_libcore_util_NativeAllocationRegistry_applyFreeFunction(JNIEnv* env, jclass cls, jlong function, jlong pointer) {
    if (function == (jlong)(uintptr_t)free) free((void*)(uintptr_t)pointer);
}
JNIEXPORT jint JNICALL Java_dalvik_system_VMRuntime_getNotifyNativeInterval(JNIEnv* env, jclass cls) { return 1024; }
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_notifyNativeAllocationsInternal(JNIEnv* env, jobject self) { }
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_registerNativeAllocation(JNIEnv* env, jobject self, jlong size) { }
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_registerNativeFree(JNIEnv* env, jobject self, jlong size) { }

struct host_parcel { jint position, size; jboolean allow_fds; };
static struct host_parcel* parcel(jlong pointer) { return (struct host_parcel*)(uintptr_t)pointer; }
static void advance(jlong pointer, jint bytes) {
    struct host_parcel* p = parcel(pointer); p->position += (bytes + 3) & ~3;
    if (p->position > p->size) p->size = p->position;
}
JNIEXPORT jlong JNICALL Java_android_os_Parcel_nativeCreate(JNIEnv* env, jclass cls) {
    struct host_parcel* p = calloc(1, sizeof(*p)); p->allow_fds = JNI_TRUE; return (jlong)(uintptr_t)p;
}
JNIEXPORT void JNICALL Java_android_os_Parcel_nativeFreeBuffer(JNIEnv* env, jclass cls, jlong pointer) {
    parcel(pointer)->position = parcel(pointer)->size = 0;
}
JNIEXPORT void JNICALL Java_android_os_Parcel_nativeDestroy(JNIEnv* env, jclass cls, jlong pointer) { free(parcel(pointer)); }
JNIEXPORT jint JNICALL Java_android_os_Parcel_nativeDataPosition(JNIEnv* env, jclass cls, jlong pointer) { return parcel(pointer)->position; }
JNIEXPORT jint JNICALL Java_android_os_Parcel_nativeDataSize(JNIEnv* env, jclass cls, jlong pointer) { return parcel(pointer)->size; }
JNIEXPORT void JNICALL Java_android_os_Parcel_nativeSetDataPosition(JNIEnv* env, jclass cls, jlong pointer, jint position) { parcel(pointer)->position = position; }
JNIEXPORT void JNICALL Java_android_os_Parcel_nativeSetDataSize(JNIEnv* env, jclass cls, jlong pointer, jint size) { parcel(pointer)->size = size; }
JNIEXPORT jboolean JNICALL Java_android_os_Parcel_nativePushAllowFds(JNIEnv* env, jclass cls, jlong pointer, jboolean allow) {
    jboolean previous = parcel(pointer)->allow_fds; parcel(pointer)->allow_fds = allow; return previous;
}
JNIEXPORT void JNICALL Java_android_os_Parcel_nativeRestoreAllowFds(JNIEnv* env, jclass cls, jlong pointer, jboolean previous) { parcel(pointer)->allow_fds = previous; }
JNIEXPORT jboolean JNICALL Java_android_os_Parcel_nativeHasFileDescriptors(JNIEnv* env, jclass cls, jlong pointer) { return JNI_FALSE; }
JNIEXPORT jint JNICALL Java_android_os_Parcel_nativeWriteInt(JNIEnv* env, jclass cls, jlong pointer, jint value) { advance(pointer, 4); return 0; }
JNIEXPORT jint JNICALL Java_android_os_Parcel_nativeWriteLong(JNIEnv* env, jclass cls, jlong pointer, jlong value) { advance(pointer, 8); return 0; }
JNIEXPORT void JNICALL Java_android_os_Parcel_nativeWriteString8(JNIEnv* env, jclass cls, jlong pointer, jstring value) {
    advance(pointer, 4); if (value) advance(pointer, (*env)->GetStringUTFLength(env, value) + 1);
}
JNIEXPORT void JNICALL Java_android_os_Parcel_nativeWriteString16(JNIEnv* env, jclass cls, jlong pointer, jstring value) {
    advance(pointer, 4); if (value) advance(pointer, ((*env)->GetStringLength(env, value) + 1) * 2);
}
JNIEXPORT void JNICALL Java_android_os_Parcel_nativeWriteByteArray(JNIEnv* env, jclass cls, jlong pointer, jbyteArray bytes, jint offset, jint count) { advance(pointer, 4); advance(pointer, count); }
JNIEXPORT void JNICALL Java_android_os_Parcel_nativeMarkSensitive(JNIEnv* env, jclass cls, jlong pointer) { }
