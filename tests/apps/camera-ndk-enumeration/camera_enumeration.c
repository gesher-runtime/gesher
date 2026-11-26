/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#include <camera/NdkCameraManager.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <string.h>

typedef camera_status_t (*GetIdList)(ACameraManager*, ACameraIdList**);

struct ThreadQuery {
    ACameraManager* manager;
    GetIdList get;
    ACameraIdList* ids;
    camera_status_t status;
};

static void* query_in_native_thread(void* arg) {
    struct ThreadQuery* query = arg;
    query->status = query->get(query->manager, &query->ids);
    return NULL;
}

JNIEXPORT jobjectArray JNICALL Java_org_gesher_contracts_camerandk_ProbeApplication_enumerate(
        JNIEnv* env, jclass ignored) {
    (void) ignored;
    const char* failure = NULL;
    ACameraManager* manager = NULL;
    ACameraIdList* first = NULL;
    struct ThreadQuery query = {0};
    jobjectArray output = NULL;
    void* library = dlopen("libcamera2ndk.so", RTLD_NOW | RTLD_LOCAL);
#define REQUIRE(condition, explanation) do { if (!(condition)) { failure = explanation; goto done; } } while (0)
    REQUIRE(library != NULL, "cannot dlopen libcamera2ndk.so");
    query.get = (GetIdList) dlsym(library, "ACameraManager_getCameraIdList");
    REQUIRE(query.get != NULL, "camera-ID discovery symbol missing");
    manager = ACameraManager_create();
    REQUIRE(manager != NULL, "cannot allocate camera manager");

    ACameraIdList sentinel = {0};
    ACameraIdList* invalid = &sentinel;
    REQUIRE(ACameraManager_getCameraIdList(NULL, &invalid) == ACAMERA_ERROR_INVALID_PARAMETER
            && invalid == &sentinel, "null manager did not preserve output");
    REQUIRE(ACameraManager_getCameraIdList(manager, NULL) == ACAMERA_ERROR_INVALID_PARAMETER,
            "null output did not fail");
    REQUIRE(ACameraManager_getCameraIdList(manager, &first) == ACAMERA_OK && first != NULL,
            "camera enumeration failed");

    pthread_t thread;
    query.manager = manager;
    REQUIRE(pthread_create(&thread, NULL, query_in_native_thread, &query) == 0,
            "cannot create discovery thread");
    REQUIRE(pthread_join(thread, NULL) == 0, "cannot join discovery thread");
    REQUIRE(query.status == ACAMERA_OK && query.ids != NULL && query.ids != first
            && query.ids->numCameras == first->numCameras, "native-thread camera discovery failed");

    ACameraManager_delete(manager);
    manager = NULL;
    for (int i = 0; i < first->numCameras; ++i) {
        REQUIRE(first->cameraIds[i] != query.ids->cameraIds[i]
                && strcmp(first->cameraIds[i], query.ids->cameraIds[i]) == 0,
                "camera ID storage was shared or changed");
    }
    ACameraManager_deleteCameraIdList(first);
    first = NULL;

    jclass string = (*env)->FindClass(env, "java/lang/String");
    if (string == NULL) goto done;
    output = (*env)->NewObjectArray(env, query.ids->numCameras, string, NULL);
    if (output == NULL) goto done;
    for (int i = 0; i < query.ids->numCameras; ++i) {
        jstring id = (*env)->NewStringUTF(env, query.ids->cameraIds[i]);
        if (id == NULL) goto done;
        (*env)->SetObjectArrayElement(env, output, i, id);
        (*env)->DeleteLocalRef(env, id);
        if ((*env)->ExceptionCheck(env)) goto done;
    }

done:
    ACameraManager_delete(manager);
    ACameraManager_deleteCameraIdList(first);
    ACameraManager_deleteCameraIdList(query.ids);
    if (library != NULL) dlclose(library);
    if (failure != NULL) {
        jclass error = (*env)->FindClass(env, "java/lang/AssertionError");
        if (error != NULL) (*env)->ThrowNew(env, error, failure);
        return NULL;
    }
    return output;
#undef REQUIRE
}
