/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <jni.h>
#include <stdint.h>

namespace gesher::nativeloader {

void Initialize();
// ART invokes this after JavaVM destruction. It clears native registry state
// without invoking JNI; namespace identities and live ELF objects are retained.
// Calls using the retired VM must have stopped before this lifecycle boundary.
void Reset();
// Called only by the actual VM application-target initializer, independently
// of an individual classloader namespace's compatibility target.
void SetApplicationTargetSdkVersion(uint32_t version);
jstring CreateClassLoaderNamespace(JNIEnv* env, int32_t target_sdk_version,
                                   jobject class_loader, bool is_shared, jstring dex_path,
                                   jstring library_path, jstring permitted_path,
                                   jstring uses_library_list);
void* OpenNativeLibrary(JNIEnv* env, int32_t target_sdk_version, const char* path,
                        jobject class_loader, const char* caller_location, jstring library_path,
                        bool* needs_native_bridge, char** error_msg);

// Each copied row is {SONAME, ndk|device|private, x86_64|arm64-v8a}.
jobjectArray BuiltinCatalog(JNIEnv* env);
jobjectArray PublicFileCatalog(JNIEnv* env);

}  // namespace gesher::nativeloader
