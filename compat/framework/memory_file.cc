// Keep AOSP's MemoryFile operations; only the registration table needs a
// System V entry point on Gesher, as in services_natives.cc.
#include <linux/ashmem.h>
#include "android_os_MemoryFile.cpp"

namespace android {
int register_gesher_MemoryFile(JNIEnv* env) {
    static const JNINativeMethod kMethods[] = {
        {"native_pin", "(Ljava/io/FileDescriptor;Z)Z", GESHER_JNI(android_os_MemoryFile_pin)},
        {"native_get_size", "(Ljava/io/FileDescriptor;)I", GESHER_JNI(android_os_MemoryFile_get_size)},
    };
    return jniRegisterNativeMethods(env, "android/os/MemoryFile", kMethods, NELEM(kMethods));
}
}  // namespace android
