/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Declared JNI identity/initialization boundary. No SQLite or Binder transport
 * emulation: this host fixture checks actual Java denial/metadata only. */
#define Java_libcore_io_Linux_nativeGetuid dictionary_original_getuid
#include "../scheduler/framework_host.c"
#undef Java_libcore_io_Linux_nativeGetuid
JNIEXPORT jint JNICALL Java_libcore_io_Linux_nativeGetuid(JNIEnv* env, jclass cls) {
    return 10000; /* Deliberately reproduce the legacy shared guest UID. */
}
