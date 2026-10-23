// Gesher: libcore's native libraries, linked into the executable.
//
// ART loads libicu_jni.so, libjavacore.so and libopenjdk.so (libopenjdkd.so
// in debug builds) at startup with dlopen and calls their JNI_OnLoad
// (runtime.cc). Each is registered here as a built-in library of gesher-base's
// dlopen (<gesher/builtin_library.h>), with its JNI_OnLoad under the name it
// was compiled with (cmake/aosp/libcore_native.cmake, cmake/aosp/icu.cmake).

#include <gesher/builtin_library.h>

#include <jni.h>

extern "C" {
jint JNI_OnLoad_icu_jni(JavaVM* vm, void* reserved);
void JNI_OnUnload_icu_jni(JavaVM* vm, void* reserved);
jint JNI_OnLoad_javacore(JavaVM* vm, void* reserved);
void JNI_OnUnload_javacore(JavaVM* vm, void* reserved);
jint JNI_OnLoad_openjdk(JavaVM* vm, void* reserved);

// EPoll.c's and EPollPort.c's natives, found by name.
JNIEXPORT jint JNICALL Java_sun_nio_ch_EPoll_eventSize(JNIEnv*, jclass);
JNIEXPORT jint JNICALL Java_sun_nio_ch_EPoll_eventsOffset(JNIEnv*, jclass);
JNIEXPORT jint JNICALL Java_sun_nio_ch_EPoll_dataOffset(JNIEnv*, jclass);
JNIEXPORT jint JNICALL Java_sun_nio_ch_EPoll_epollCreate(JNIEnv*, jclass);
JNIEXPORT jint JNICALL Java_sun_nio_ch_EPoll_epollCtl(JNIEnv*, jclass, jint, jint, jint, jint);
JNIEXPORT jint JNICALL Java_sun_nio_ch_EPoll_epollWait(JNIEnv*, jclass, jint, jlong, jint);
JNIEXPORT void JNICALL Java_sun_nio_ch_EPoll_close0(JNIEnv*, jclass, jint);
JNIEXPORT void JNICALL Java_sun_nio_ch_EPollPort_socketpair(JNIEnv*, jclass, jintArray);
JNIEXPORT void JNICALL Java_sun_nio_ch_EPollPort_interrupt(JNIEnv*, jclass, jint);
JNIEXPORT void JNICALL Java_sun_nio_ch_EPollPort_drain1(JNIEnv*, jclass, jint);
JNIEXPORT void JNICALL Java_sun_nio_ch_EPollPort_close0(JNIEnv*, jclass, jint);
}

namespace {

const gesher_builtin_symbol kIcuJni[] = {
    {"JNI_OnLoad", reinterpret_cast<void*>(&JNI_OnLoad_icu_jni)},
    {"JNI_OnUnload", reinterpret_cast<void*>(&JNI_OnUnload_icu_jni)},
};

const gesher_builtin_symbol kJavacore[] = {
    {"JNI_OnLoad", reinterpret_cast<void*>(&JNI_OnLoad_javacore)},
    {"JNI_OnUnload", reinterpret_cast<void*>(&JNI_OnUnload_javacore)},
};

const gesher_builtin_symbol kOpenjdk[] = {
    {"JNI_OnLoad", reinterpret_cast<void*>(&JNI_OnLoad_openjdk)},
    {"Java_sun_nio_ch_EPoll_eventSize", reinterpret_cast<void*>(&Java_sun_nio_ch_EPoll_eventSize)},
    {"Java_sun_nio_ch_EPoll_eventsOffset",
     reinterpret_cast<void*>(&Java_sun_nio_ch_EPoll_eventsOffset)},
    {"Java_sun_nio_ch_EPoll_dataOffset", reinterpret_cast<void*>(&Java_sun_nio_ch_EPoll_dataOffset)},
    {"Java_sun_nio_ch_EPoll_epollCreate",
     reinterpret_cast<void*>(&Java_sun_nio_ch_EPoll_epollCreate)},
    {"Java_sun_nio_ch_EPoll_epollCtl", reinterpret_cast<void*>(&Java_sun_nio_ch_EPoll_epollCtl)},
    {"Java_sun_nio_ch_EPoll_epollWait", reinterpret_cast<void*>(&Java_sun_nio_ch_EPoll_epollWait)},
    {"Java_sun_nio_ch_EPoll_close0", reinterpret_cast<void*>(&Java_sun_nio_ch_EPoll_close0)},
    {"Java_sun_nio_ch_EPollPort_socketpair",
     reinterpret_cast<void*>(&Java_sun_nio_ch_EPollPort_socketpair)},
    {"Java_sun_nio_ch_EPollPort_interrupt",
     reinterpret_cast<void*>(&Java_sun_nio_ch_EPollPort_interrupt)},
    {"Java_sun_nio_ch_EPollPort_drain1", reinterpret_cast<void*>(&Java_sun_nio_ch_EPollPort_drain1)},
    {"Java_sun_nio_ch_EPollPort_close0", reinterpret_cast<void*>(&Java_sun_nio_ch_EPollPort_close0)},
};

const bool registered = [] {
  gesher_register_builtin_library("libicu_jni.so", kIcuJni, sizeof(kIcuJni) / sizeof(kIcuJni[0]));
  gesher_register_builtin_library("libjavacore.so", kJavacore,
                                  sizeof(kJavacore) / sizeof(kJavacore[0]));
  gesher_register_builtin_library("libopenjdk.so", kOpenjdk, sizeof(kOpenjdk) / sizeof(kOpenjdk[0]));
  gesher_register_builtin_library("libopenjdkd.so", kOpenjdk,
                                  sizeof(kOpenjdk) / sizeof(kOpenjdk[0]));
  return true;
}();

}  // namespace
