/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Host-only JNI ownership boundary. The real ApkAssets/AssetManager/split-loader Java is
 * unchanged; this ledger owns actual Linux fd/mmap resources, checks destruction order,
 * and injects native configuration failures. It does not decode Android APK resources or
 * prove Windows section/truncation behavior; the separate ART fixture does those operations. */
#include "framework_host.c"
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>

enum { MAX_RECORDS = 128, MAX_MANAGER_ASSETS = 16 };
typedef struct {
    int live;
    int fd;
    int system;
    size_t length;
    void* mapping;
    char* path;
} Asset;
typedef struct {
    int live;
    size_t count;
    Asset* assets[MAX_MANAGER_ASSETS];
} Manager;
static Asset* assets[MAX_RECORDS];
static Manager* managers[MAX_RECORDS];
static size_t asset_count, manager_count;
static int failure_stage;

static void fail(JNIEnv* env, const char* type, const char* message) {
    (*env)->ThrowNew(env, (*env)->FindClass(env, type), message);
}

JNIEXPORT jlong JNICALL Java_android_content_res_ApkAssets_nativeLoad(
        JNIEnv* env, jclass type, jint format, jstring path, jint flags, jobject provider) {
    const char* name = (*env)->GetStringUTFChars(env, path, NULL);
    if (!name) return 0;
    int fd = open(name, O_RDONLY);
    struct stat status;
    if (fd < 0 || fstat(fd, &status) || !status.st_size) {
        if (fd >= 0) close(fd);
        (*env)->ReleaseStringUTFChars(env, path, name);
        fail(env, "java/io/IOException", "Host APK source cannot be mapped");
        return 0;
    }
    void* view = mmap(NULL, (size_t)status.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (view == MAP_FAILED || asset_count == MAX_RECORDS) {
        if (view != MAP_FAILED) munmap(view, (size_t)status.st_size);
        close(fd);
        (*env)->ReleaseStringUTFChars(env, path, name);
        fail(env, "java/lang/AssertionError", "Host mapping allocation failed");
        return 0;
    }
    Asset* asset = calloc(1, sizeof(*asset));
    if (!asset) abort();
    asset->live = 1; asset->fd = fd; asset->system = (flags & 1) != 0;
    asset->mapping = view; asset->length = (size_t)status.st_size; asset->path = strdup(name);
    assets[asset_count++] = asset;
    (*env)->ReleaseStringUTFChars(env, path, name);
    return (jlong)(intptr_t)asset;
}

JNIEXPORT jlong JNICALL Java_android_content_res_ApkAssets_nativeGetStringBlock(
        JNIEnv* env, jclass type, jlong pointer) {
    return pointer; /* StringBlock borrows this pool; no host string reads are requested. */
}

JNIEXPORT void JNICALL Java_android_content_res_ApkAssets_nativeDestroy(
        JNIEnv* env, jclass type, jlong pointer) {
    Asset* asset = (Asset*)(intptr_t)pointer;
    if (!asset || !asset->live) {
        fail(env, "java/lang/AssertionError", "Native APK asset destroyed twice"); return;
    }
    for (size_t i = 0; i < manager_count; i++) {
        if (!managers[i]->live) continue;
        for (size_t j = 0; j < managers[i]->count; j++) {
            if (managers[i]->assets[j] == asset) {
                fail(env, "java/lang/AssertionError", "APK asset destroyed before its manager"); return;
            }
        }
    }
    if (munmap(asset->mapping, asset->length) || close(asset->fd)) {
        fail(env, "java/lang/AssertionError", "Host mapping close failed"); return;
    }
    asset->live = 0;
}

JNIEXPORT jstring JNICALL Java_android_content_res_ApkAssets_nativeGetAssetPath(
        JNIEnv* env, jclass type, jlong pointer) {
    Asset* asset = (Asset*)(intptr_t)pointer;
    return asset && asset->live ? (*env)->NewStringUTF(env, asset->path) : NULL;
}

JNIEXPORT jstring JNICALL Java_android_content_res_ApkAssets_nativeGetDebugName(
        JNIEnv* env, jclass type, jlong pointer) {
    return Java_android_content_res_ApkAssets_nativeGetAssetPath(env, type, pointer);
}

JNIEXPORT jlong JNICALL Java_android_content_res_AssetManager_nativeCreate(JNIEnv* env, jclass type) {
    if (manager_count == MAX_RECORDS) {
        fail(env, "java/lang/AssertionError", "Host manager allocation exhausted"); return 0;
    }
    Manager* manager = calloc(1, sizeof(*manager));
    if (!manager) abort();
    manager->live = 1; managers[manager_count++] = manager;
    return (jlong)(intptr_t)manager;
}

JNIEXPORT void JNICALL Java_android_content_res_AssetManager_nativeDestroy(
        JNIEnv* env, jclass type, jlong pointer) {
    Manager* manager = (Manager*)(intptr_t)pointer;
    if (!manager || !manager->live) {
        fail(env, "java/lang/AssertionError", "Native manager destroyed twice"); return;
    }
    manager->live = 0;
}

JNIEXPORT void JNICALL Java_android_content_res_AssetManager_nativeSetApkAssets(
        JNIEnv* env, jclass type, jlong pointer, jobjectArray values, jboolean invalidate, jboolean preset) {
    Manager* manager = (Manager*)(intptr_t)pointer;
    jsize count = (*env)->GetArrayLength(env, values);
    if (!manager->live || count > MAX_MANAGER_ASSETS) {
        fail(env, "java/lang/AssertionError", "Invalid host manager asset assignment"); return;
    }
    int has_private = 0;
    manager->count = (size_t)count;
    jfieldID native = (*env)->GetFieldID(env, (*env)->FindClass(env, "android/content/res/ApkAssets"),
            "mNativePtr", "J");
    for (jsize i = 0; i < count; i++) {
        jobject value = (*env)->GetObjectArrayElement(env, values, i);
        Asset* asset = (Asset*)(intptr_t)(*env)->GetLongField(env, value, native);
        (*env)->DeleteLocalRef(env, value);
        if (!asset || !asset->live) {
            fail(env, "java/lang/AssertionError", "Manager received a closed APK asset"); return;
        }
        manager->assets[i] = asset;
        if (!asset->system) has_private = 1;
    }
    if (failure_stage == 2 && has_private) {
        failure_stage = 0;
        fail(env, "java/lang/IllegalStateException", "host manager setup failure 2");
    }
}

JNIEXPORT void JNICALL Java_android_content_res_AssetManager_nativeSetConfiguration(
        JNIEnv* env, jclass type, jlong pointer, jint mcc, jint mnc, jstring locale,
        jobjectArray locales, jint orientation, jint touchscreen, jint density, jint keyboard,
        jint keyboard_hidden, jint navigation, jint screen_width, jint screen_height,
        jint smallest_width, jint width_dp, jint height_dp, jint layout, jint ui_mode,
        jint color_mode, jint gender, jint major, jint minor, jboolean refresh) {
    if (failure_stage == 1) {
        failure_stage = 0;
        fail(env, "java/lang/IllegalStateException", "host manager setup failure 1");
    }
}

JNIEXPORT jint JNICALL Java_ParserAssetLifecycleHost_liveAssets(JNIEnv* env, jclass type) {
    int count = 0;
    for (size_t i = 0; i < asset_count; i++) count += assets[i]->live;
    return count;
}
JNIEXPORT jint JNICALL Java_ParserAssetLifecycleHost_liveManagers(JNIEnv* env, jclass type) {
    int count = 0;
    for (size_t i = 0; i < manager_count; i++) count += managers[i]->live;
    return count;
}
JNIEXPORT void JNICALL Java_ParserAssetLifecycleHost_failSetup(JNIEnv* env, jclass type, jint stage) {
    failure_stage = stage;
}
