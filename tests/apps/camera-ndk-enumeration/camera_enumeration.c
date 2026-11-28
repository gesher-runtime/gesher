/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#include <camera/NdkCameraManager.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
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
    // Gesher keeps dlopen("libcamera2ndk.so") unavailable until capture
    // sessions exist; the linked library's symbols are global.
#define REQUIRE(condition, explanation) do { if (!(condition)) { failure = explanation; goto done; } } while (0)
    query.get = (GetIdList) dlsym(RTLD_DEFAULT, "ACameraManager_getCameraIdList");
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
    if (failure != NULL) {
        jclass error = (*env)->FindClass(env, "java/lang/AssertionError");
        if (error != NULL) (*env)->ThrowNew(env, error, failure);
        return NULL;
    }
    return output;
#undef REQUIRE
}


static void disconnected(void* context, ACameraDevice* device) {
    (void) device;
    ++*(_Atomic int*)context;
}

static void device_error(void* context, ACameraDevice* device, int error) {
    (void) device;
    (void) error;
    ++*(_Atomic int*)context;
}

JNIEXPORT void JNICALL Java_org_gesher_contracts_camerandk_EnumerationActivity_verifyOpen(
        JNIEnv* env, jclass ignored, jstring selected, jboolean granted) {
    (void) ignored;
    const char* failure = NULL;
    const char* id = selected != NULL ? (*env)->GetStringUTFChars(env, selected, NULL) : NULL;
    if (selected != NULL && id == NULL) return;
    ACameraManager* manager = ACameraManager_create();
    ACameraDevice* device = NULL;
    _Atomic int errors = 0;
    ACameraDevice_StateCallbacks callbacks = {
        .context = &errors, .onDisconnected = disconnected, .onError = device_error,
    };
    typedef camera_status_t (*Open)(ACameraManager*, const char*, ACameraDevice_StateCallbacks*,
                                    ACameraDevice**);
    Open open = (Open)dlsym(RTLD_DEFAULT, "ACameraManager_openCamera");
#define REQUIRE(condition, explanation) do { if (!(condition)) { failure = explanation; goto done; } } while (0)
    REQUIRE(manager != NULL && open != NULL, "cannot resolve camera device open");
    REQUIRE(ACameraManager_openCamera(NULL, "gesher-absent-camera", &callbacks, &device)
            == ACAMERA_ERROR_INVALID_PARAMETER && device == NULL, "null manager did not fail");
    REQUIRE(open(manager, "gesher-absent-camera", &callbacks, &device)
            == ACAMERA_ERROR_INVALID_PARAMETER && device == NULL, "unknown camera did not fail");
    REQUIRE(ACameraDevice_close(NULL) == ACAMERA_ERROR_INVALID_PARAMETER, "null close did not fail");
    if (id == NULL) goto done;
    if (!granted) {
        REQUIRE(open(manager, id, &callbacks, &device) == ACAMERA_ERROR_PERMISSION_DENIED
                && device == NULL, "app without camera permission was not denied");
        goto done;
    }

    REQUIRE(open(manager, id, &callbacks, &device) == ACAMERA_OK && device != NULL,
            "granted native device open failed");
    ACameraManager_delete(manager);
    manager = NULL;
    const char* opened_id = ACameraDevice_getId(device);
    REQUIRE(opened_id != NULL && strcmp(opened_id, id) == 0, "camera ID was not retained");
    camera_status_t close_status = ACameraDevice_close(device);
    device = NULL;
    REQUIRE(close_status == ACAMERA_OK, "camera close failed");
    manager = ACameraManager_create();
    REQUIRE(manager != NULL && ACameraManager_openCamera(manager, id, &callbacks, &device)
            == ACAMERA_OK && device != NULL, "cannot reopen the closed camera");
    close_status = ACameraDevice_close(device);
    device = NULL;
    REQUIRE(close_status == ACAMERA_OK && errors == 0, "ordinary close posted a device error");
done:
    if (device != NULL) ACameraDevice_close(device);
    ACameraManager_delete(manager);
    if (id != NULL) (*env)->ReleaseStringUTFChars(env, selected, id);
    if (failure != NULL) {
        jclass error = (*env)->FindClass(env, "java/lang/AssertionError");
        if (error != NULL) (*env)->ThrowNew(env, error, failure);
    }
#undef REQUIRE
}
