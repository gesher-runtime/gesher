/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Native initialization/identity boundaries only, shared with the URI policy fixture. */
#include "../uri/framework_host.c"
/* A real Bundle emits its ordinary type-warning before trying a single URI. */
JNIEXPORT jint JNICALL Java_android_util_Log_logger_1entry_1max_1payload_1native(JNIEnv* env,jclass cls) {
    return 4068;
}
