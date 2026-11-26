// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include <android/hardware_buffer.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <stdbool.h>
#include <media/NdkImageReader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "NDK YUV image reader failed at line %d: %s\n", __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

enum { kWidth = 64, kHeight = 48 };

static unsigned sample(const uint8_t* data, int row_stride, int pixel_stride, int plane) {
    const int x = plane == 0 ? kWidth / 2 : kWidth / 4;
    const int y = plane == 0 ? kHeight / 2 : kHeight / 4;
    return data[y * row_stride + x * pixel_stride];
}

static void check_frame(AImage* image, int color, bool check_buffer) {
    // The reference clip uses limited-range BT.601 red followed by blue.
    static const int expected[2][3] = {{81, 90, 240}, {41, 240, 110}};
    int32_t width = 0, height = 0, format = 0, count = 0;
    AImageCropRect crop = {0};
    CHECK(AImage_getWidth(image, &width) == AMEDIA_OK && width == kWidth);
    CHECK(AImage_getHeight(image, &height) == AMEDIA_OK && height == kHeight);
    CHECK(AImage_getFormat(image, &format) == AMEDIA_OK && format == AIMAGE_FORMAT_YUV_420_888);
    CHECK(AImage_getCropRect(image, &crop) == AMEDIA_OK);
    CHECK(crop.left == 0 && crop.top == 0 && crop.right == kWidth && crop.bottom == kHeight);
    CHECK(AImage_getNumberOfPlanes(image, &count) == AMEDIA_OK && count == 3);
    // AImage holds its mapper lock once a CPU plane is requested. Read the
    // buffer first, then use AImage's own access to cross-check the samples.
    int buffer_values[3];
    if (check_buffer) {
        AHardwareBuffer* buffer = NULL;
        CHECK(AImage_getHardwareBuffer(image, &buffer) == AMEDIA_OK && buffer != NULL);
        AHardwareBuffer_Desc desc = {0};
        AHardwareBuffer_describe(buffer, &desc);
        CHECK(desc.width >= kWidth && desc.height >= kHeight && desc.layers == 1);
        AHardwareBuffer_Planes planes = {0};
        int result = AHardwareBuffer_lockPlanes(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
                                                -1, NULL, &planes);
        if (result != 0) {
            fprintf(stderr, "YUV hardware buffer lockPlanes returned %d (format %#x, usage %#llx)\n",
                    result, desc.format, (unsigned long long)desc.usage);
        }
        CHECK(result == 0 && planes.planeCount == 3);
        for (int plane = 0; plane < 3; ++plane) {
            const AHardwareBuffer_Plane* layout = &planes.planes[plane];
            CHECK(layout->data != NULL && layout->rowStride > 0 && layout->pixelStride > 0);
            buffer_values[plane] = (int)sample(layout->data, layout->rowStride, layout->pixelStride,
                                               plane);
        }
        CHECK(AHardwareBuffer_unlock(buffer, NULL) == 0);
    }

    int values[3];
    int32_t chroma_pixel_stride = 0, chroma_row_stride = 0;
    for (int plane = 0; plane < 3; ++plane) {
        int32_t row_stride = 0, pixel_stride = 0;
        uint8_t* data = NULL;
        int length = 0;
        CHECK(AImage_getPlanePixelStride(image, plane, &pixel_stride) == AMEDIA_OK);
        CHECK(AImage_getPlaneRowStride(image, plane, &row_stride) == AMEDIA_OK);
        CHECK(AImage_getPlaneData(image, plane, &data, &length) == AMEDIA_OK);
        if (plane == 0) CHECK(pixel_stride == 1);
        if (plane == 1) {
            chroma_pixel_stride = pixel_stride;
            chroma_row_stride = row_stride;
        }
        if (plane == 2) {
            CHECK(pixel_stride == chroma_pixel_stride && row_stride == chroma_row_stride);
        }
        int columns = plane == 0 ? kWidth : kWidth / 2;
        int rows = plane == 0 ? kHeight : kHeight / 2;
        CHECK(pixel_stride > 0 && row_stride >= (columns - 1) * pixel_stride + 1);
        CHECK(data != NULL && length >= (rows - 1) * row_stride + (columns - 1) * pixel_stride + 1);
        values[plane] = (int)sample(data, row_stride, pixel_stride, plane);
        if (check_buffer) CHECK(values[plane] == buffer_values[plane]);
        if (abs(values[plane] - expected[color][plane]) > 10) {
            fprintf(stderr, "YUV frame %d plane %d: expected %d, got %d\n",
                    color, plane, expected[color][plane], values[plane]);
            exit(1);
        }
    }

    printf("NDK YUV image reader: frame %d YUV %d %d %d\n", color, values[0], values[1], values[2]);
}

JNIEXPORT jlong JNICALL Java_gesher_NdkYuvImageReaderTest_createReader(JNIEnv* env, jclass cls) {
    (void)env;
    (void)cls;
    AImageReader* reader = NULL;
    CHECK(AImageReader_new(kWidth, kHeight, AIMAGE_FORMAT_YUV_420_888, 2, &reader) == AMEDIA_OK);
    CHECK(reader != NULL);
    int32_t format = 0;
    CHECK(AImageReader_getFormat(reader, &format) == AMEDIA_OK && format == AIMAGE_FORMAT_YUV_420_888);
    return (jlong)(uintptr_t)reader;
}

JNIEXPORT jobject JNICALL Java_gesher_NdkYuvImageReaderTest_getSurface(JNIEnv* env, jclass cls,
                                                                    jlong handle) {
    (void)cls;
    ANativeWindow* window = NULL;
    CHECK(AImageReader_getWindow((AImageReader*)(uintptr_t)handle, &window) == AMEDIA_OK);
    jobject surface = ANativeWindow_toSurface(env, window);
    CHECK(surface != NULL);
    return surface;
}

JNIEXPORT jlong JNICALL Java_gesher_NdkYuvImageReaderTest_checkImage(JNIEnv* env, jclass cls,
                                                                  jlong handle, jlong timestamp,
                                                                  jint color) {
    (void)env;
    (void)cls;
    AImage* image = NULL;
    media_status_t result = AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE;
    for (int attempt = 0; attempt < 500 && result == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE; ++attempt) {
        result = AImageReader_acquireNextImage((AImageReader*)(uintptr_t)handle, &image);
        if (result == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
            struct timespec delay = {.tv_nsec = 10000000};
            nanosleep(&delay, NULL);
        }
    }
    CHECK(result == AMEDIA_OK && image != NULL);
    int64_t actual_timestamp = 0;
    CHECK(AImage_getTimestamp(image, &actual_timestamp) == AMEDIA_OK);
    CHECK(actual_timestamp == timestamp);
    check_frame(image, color, true);
    return (jlong)(uintptr_t)image;
}

JNIEXPORT void JNICALL Java_gesher_NdkYuvImageReaderTest_checkRetainedImage(JNIEnv* env, jclass cls,
                                                                         jlong handle) {
    (void)env;
    (void)cls;
    check_frame((AImage*)(uintptr_t)handle, 0, false);
}

JNIEXPORT void JNICALL Java_gesher_NdkYuvImageReaderTest_deleteImage(JNIEnv* env, jclass cls,
                                                                   jlong handle) {
    (void)env;
    (void)cls;
    AImage_delete((AImage*)(uintptr_t)handle);
}

JNIEXPORT void JNICALL Java_gesher_NdkYuvImageReaderTest_deleteReader(JNIEnv* env, jclass cls,
                                                                    jlong handle) {
    (void)env;
    (void)cls;
    AImageReader_delete((AImageReader*)(uintptr_t)handle);
}
