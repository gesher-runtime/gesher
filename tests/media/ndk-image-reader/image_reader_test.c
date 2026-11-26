// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <media/NdkImageReader.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(IMAGE_READER_STANDALONE) && !defined(IMAGE_READER_COMPONENT)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#endif

#ifndef IMAGE_READER_STANDALONE
#include <jni.h>
#endif

enum { kWidth = 6, kHeight = 4 };
static int checks;
#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        fprintf(stderr, "NDK image reader check failed at line %d: %s\n", __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

static uint32_t pixel(uint32_t color, int x, int y) {
    return color | (uint32_t)x | ((uint32_t)y << 8);
}

static uint32_t read_pixel(const uint8_t* pixels, int row_stride, int x, int y) {
    uint32_t result;
    memcpy(&result, pixels + y * row_stride + x * 4, sizeof(result));
    return result;
}

static void post(ANativeWindow* window, uint32_t color) {
    ANativeWindow_Buffer buffer = {0};
    CHECK(ANativeWindow_lock(window, &buffer, NULL) == 0);
    CHECK(buffer.width == kWidth && buffer.height == kHeight);
    CHECK(buffer.stride >= kWidth && buffer.format == AIMAGE_FORMAT_RGBA_8888);
    CHECK(buffer.bits != NULL);
    for (int y = 0; y < buffer.height; ++y) {
        uint32_t* row = (uint32_t*)buffer.bits + y * buffer.stride;
        for (int x = 0; x < buffer.width; ++x) {
            row[x] = pixel(color, x, y);
        }
    }
    CHECK(ANativeWindow_unlockAndPost(window) == 0);
}

static void wait_for_pixels(int fence) {
    CHECK(fence >= -1);
    if (fence >= 0) {
        struct pollfd poll_fd = {fence, POLLIN, 0};
        CHECK(poll(&poll_fd, 1, 5000) == 1 && (poll_fd.revents & POLLIN) != 0);
        CHECK(close(fence) == 0);
    }
}

static void expect_image(AImage* image, uint32_t color) {
    int32_t value = 0;
    int32_t row_stride = 0;
    int64_t timestamp = 0;
    uint8_t* data = NULL;
    int length = 0;
    AImageCropRect crop = {0};

    CHECK(image != NULL);
    CHECK(AImage_getWidth(image, &value) == AMEDIA_OK && value == kWidth);
    CHECK(AImage_getHeight(image, &value) == AMEDIA_OK && value == kHeight);
    CHECK(AImage_getFormat(image, &value) == AMEDIA_OK && value == AIMAGE_FORMAT_RGBA_8888);
    CHECK(AImage_getCropRect(image, &crop) == AMEDIA_OK);
    CHECK(crop.left == 0 && crop.top == 0 && crop.right == kWidth && crop.bottom == kHeight);
    CHECK(AImage_getTimestamp(image, &timestamp) == AMEDIA_OK && timestamp > 0);
    CHECK(AImage_getNumberOfPlanes(image, &value) == AMEDIA_OK && value == 1);
    CHECK(AImage_getPlanePixelStride(image, 0, &value) == AMEDIA_OK && value == 4);
    CHECK(AImage_getPlaneRowStride(image, 0, &row_stride) == AMEDIA_OK && row_stride >= kWidth * 4);
    CHECK(AImage_getPlaneData(image, 1, &data, &length) == AMEDIA_ERROR_INVALID_PARAMETER);
    CHECK(AImage_getPlaneData(image, 0, &data, &length) == AMEDIA_OK);
    CHECK(data != NULL && length >= (kHeight - 1) * row_stride + kWidth * 4);
    uint32_t actual = read_pixel(data, row_stride, 0, 0);
    if (actual != pixel(color, 0, 0)) {
        fprintf(stderr, "RGBA first pixel: expected %08x, got %08x (stride %d, length %d)\n",
                pixel(color, 0, 0), actual, row_stride, length);
    }
    CHECK(actual == pixel(color, 0, 0));
    CHECK(read_pixel(data, row_stride, kWidth - 1, kHeight - 1) ==
          pixel(color, kWidth - 1, kHeight - 1));
}

struct CallbackTest {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t caller;
    AImage* image;
    int status;
    int called;
    int same_thread;
};

static void on_image(void* opaque, AImageReader* reader) {
    struct CallbackTest* callback = opaque;
    AImage* image = NULL;
    const int status = AImageReader_acquireNextImage(reader, &image);
    pthread_mutex_lock(&callback->mutex);
    callback->image = image;
    callback->status = status;
    callback->same_thread = pthread_equal(callback->caller, pthread_self());
    callback->called = 1;
    pthread_cond_signal(&callback->changed);
    pthread_mutex_unlock(&callback->mutex);
}

static void expect_listener(void) {
    AImageReader* reader = NULL;
    ANativeWindow* window = NULL;
    const uint32_t color = 0xffb60000u;
    struct CallbackTest callback = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
        .caller = pthread_self(),
    };
    AImageReader_ImageListener listener = {&callback, on_image};

    CHECK(AImageReader_newWithUsage(kWidth, kHeight, AIMAGE_FORMAT_RGBA_8888,
                                   AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, 2, &reader) == AMEDIA_OK);
    CHECK(AImageReader_getWindow(reader, &window) == AMEDIA_OK && window != NULL);
    CHECK(AImageReader_setImageListener(reader, &listener) == AMEDIA_OK);
    post(window, color);

    struct timespec limit;
    CHECK(clock_gettime(CLOCK_REALTIME, &limit) == 0);
    limit.tv_sec += 5;
    CHECK(pthread_mutex_lock(&callback.mutex) == 0);
    while (!callback.called) {
        CHECK(pthread_cond_timedwait(&callback.changed, &callback.mutex, &limit) == 0);
    }
    CHECK(callback.status == AMEDIA_OK && !callback.same_thread);
    CHECK(pthread_mutex_unlock(&callback.mutex) == 0);
    expect_image(callback.image, color);
    AImage_delete(callback.image);
    CHECK(AImageReader_setImageListener(reader, NULL) == AMEDIA_OK);
    AImageReader_delete(reader);
    CHECK(pthread_cond_destroy(&callback.changed) == 0);
    CHECK(pthread_mutex_destroy(&callback.mutex) == 0);
}

#if defined(IMAGE_READER_STANDALONE) && !defined(IMAGE_READER_COMPONENT)
static int make_release_fence(void) {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) {
        puts("NDK image reader: EGL display unavailable for release-fence check");
        return -1;
    }
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (extensions == NULL || strstr(extensions, "EGL_ANDROID_native_fence_sync") == NULL) {
        CHECK(eglTerminate(display));
        puts("NDK image reader: EGL native release fence unavailable");
        return -1;
    }
    PFNEGLCREATESYNCKHRPROC create_sync =
        (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence =
        (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    PFNEGLDESTROYSYNCKHRPROC destroy_sync =
        (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    CHECK(create_sync != NULL && dup_fence != NULL && destroy_sync != NULL);

    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE,
    };
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    const EGLint surface_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    CHECK(eglChooseConfig(display, config_attributes, &config, 1, &count) && count == 1);
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
    CHECK(surface != EGL_NO_SURFACE);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    CHECK(context != EGL_NO_CONTEXT);
    CHECK(eglMakeCurrent(display, surface, surface, context));
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    EGLSyncKHR sync = create_sync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    CHECK(sync != EGL_NO_SYNC_KHR);
    glFlush();
    const int fence = dup_fence(display, sync);
    CHECK(fence >= 0);
    CHECK(destroy_sync(display, sync));
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(eglDestroyContext(display, context));
    CHECK(eglDestroySurface(display, surface));
    CHECK(eglTerminate(display));
    return fence;
}
#endif

static int run_test(void) {
    checks = 0;
    void* library = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);
    CHECK(library != NULL);
    CHECK(dlsym(library, "AImageReader_new") != NULL);
    CHECK(dlsym(library, "AImage_getHardwareBuffer") != NULL);
    CHECK(dlsym(library, "AImage_deleteAsync") != NULL);
    CHECK(dlsym(library, "AImageReader_acquireNextImageAsync") != NULL);
    CHECK(dlsym(library, "AImageReader_acquireLatestImageAsync") != NULL);
    CHECK(dlclose(library) == 0);

    AImageReader* reader = NULL;
    CHECK(AImageReader_new(kWidth, kHeight, AIMAGE_FORMAT_RGBA_8888, 2, &reader) == AMEDIA_OK);
    CHECK(reader != NULL);
    int32_t value = 0;
    CHECK(AImageReader_getWidth(reader, &value) == AMEDIA_OK && value == kWidth);
    CHECK(AImageReader_getHeight(reader, &value) == AMEDIA_OK && value == kHeight);
    CHECK(AImageReader_getMaxImages(reader, &value) == AMEDIA_OK && value == 2);
    CHECK(AImageReader_getFormat(reader, &value) == AMEDIA_OK && value == AIMAGE_FORMAT_RGBA_8888);
    CHECK(AImageReader_getWindow(reader, NULL) == AMEDIA_ERROR_INVALID_PARAMETER);

    ANativeWindow* window = NULL;
    CHECK(AImageReader_getWindow(reader, &window) == AMEDIA_OK && window != NULL);
    ANativeWindow_acquire(window);
    AImage* first = NULL;
    CHECK(AImageReader_acquireNextImage(reader, &first) == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE);
    const uint32_t red = 0xff7c0000u;
    const uint32_t green = 0xff350000u;
    const uint32_t blue = 0xffa10000u;
    post(window, red);
    int fence = -2;
    CHECK(AImageReader_acquireNextImageAsync(reader, &first, &fence) == AMEDIA_OK);
    wait_for_pixels(fence);
    expect_image(first, red);

    post(window, green);
    AImage* second = NULL;
    CHECK(AImageReader_acquireLatestImage(reader, &second) == AMEDIA_OK);
    expect_image(second, green);
    expect_image(first, red);
    post(window, blue);

    AImage* third = NULL;
    CHECK(AImageReader_acquireNextImage(reader, &third) == AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED);
    CHECK(third == NULL);
    int release_fence = -1;
#if defined(IMAGE_READER_STANDALONE) && !defined(IMAGE_READER_COMPONENT)
    release_fence = make_release_fence();
#endif
    AImage_deleteAsync(second, release_fence);
    if (release_fence >= 0) puts("NDK image reader: released through an EGL native fence");
    CHECK(AImageReader_acquireNextImage(reader, &third) == AMEDIA_OK);
    expect_image(third, blue);
    AImage_delete(third);
    AImage_delete(first);

    const uint32_t fourth = 0xff190000u;
    const uint32_t fifth = 0xffe60000u;
    post(window, fourth);
    post(window, fifth);
    AImage* latest = NULL;
    fence = -2;
    CHECK(AImageReader_acquireLatestImageAsync(reader, &latest, &fence) == AMEDIA_OK);
    wait_for_pixels(fence);
    expect_image(latest, fifth);
    AHardwareBuffer* retained = NULL;
    CHECK(AImage_getHardwareBuffer(latest, &retained) == AMEDIA_OK && retained != NULL);
    AHardwareBuffer_acquire(retained);
    AImage* missing = NULL;
    CHECK(AImageReader_acquireNextImage(reader, &missing) == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE);

    AImageReader_delete(reader);
    CHECK(AImage_getWidth(latest, &value) == AMEDIA_ERROR_INVALID_OBJECT);
    AImage_delete(latest);
    ANativeWindow_Buffer window_buffer = {0};
    CHECK(ANativeWindow_lock(window, &window_buffer, NULL) != 0);
    ANativeWindow_release(window);

    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(retained, &desc);
    CHECK(desc.width == kWidth && desc.height == kHeight && desc.stride >= kWidth);
    CHECK(desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    void* mapped = NULL;
    CHECK(AHardwareBuffer_lock(retained, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &mapped) == 0);
    CHECK(read_pixel(mapped, (int)desc.stride * 4, 0, 0) == pixel(fifth, 0, 0));
    CHECK(read_pixel(mapped, (int)desc.stride * 4, kWidth - 1, kHeight - 1) ==
          pixel(fifth, kWidth - 1, kHeight - 1));
    CHECK(AHardwareBuffer_unlock(retained, NULL) == 0);
    AHardwareBuffer_release(retained);

    expect_listener();
    printf("NDK image reader: %d checks passed\n", checks);
    return 0;
}

#ifdef IMAGE_READER_STANDALONE
int main(void) {
    return run_test();
}
#else
JNIEXPORT jint JNICALL Java_gesher_NdkImageReaderTest_nativeRun(JNIEnv* env, jclass cls) {
    (void)env;
    (void)cls;
    return run_test();
}
#endif
