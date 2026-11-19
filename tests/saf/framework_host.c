/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Explicit host-only Binder identity/allocation, tracing and native path boundary.
 * No provider, permission, filesystem operation, URI dispatch or grant substitute. */
#include "../location/framework_host.c"
JNIEXPORT jlong JNICALL Java_android_os_Trace_nativeGetEnabledTags(JNIEnv* e,jclass c){return 0;}
JNIEXPORT jboolean JNICALL Java_android_os_Trace_nativeIsTagEnabled(JNIEnv* e,jclass c,jlong tag){return JNI_FALSE;}
JNIEXPORT void JNICALL Java_android_os_Trace_nativeTraceEnd(JNIEnv* e,jclass c,jlong tag){}
JNIEXPORT jstring JNICALL Java_com_gesher_server_Zygote_nativeExePath(JNIEnv* e,jclass c){return (*e)->NewStringUTF(e,"C:\\fixture\\gesher-app.exe");}
