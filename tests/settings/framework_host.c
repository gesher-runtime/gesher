/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Real framework/provider Java and host file I/O; declared JNI initialization
 * and identity boundaries only. This path does not emulate remote Binder. */
#define Java_libcore_io_Linux_nativeGetuid settings_original_getuid
#include "../scheduler/framework_host.c"
#undef Java_libcore_io_Linux_nativeGetuid
JNIEXPORT jint JNICALL Java_libcore_io_Linux_nativeGetuid(JNIEnv* env, jclass cls) { return 1000; }
