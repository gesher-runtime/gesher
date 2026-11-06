// AOSP's shared atomic storage, with System V native entries. The existing
// host ashmem implementation supplies its file-backed mappings; it does not
// enforce the ashmem protection mask on subsequently created native mappings.
#include <atomic>
#include "android_util_MemoryIntArray.cpp"

namespace android {
int register_gesher_MemoryIntArray(JNIEnv* env) {
    static const JNINativeMethod kMethods[] = {
        {"nativeCreate", "(Ljava/lang/String;I)I", GESHER_JNI(android_util_MemoryIntArray_create)},
        {"nativeOpen", "(IZ)J", GESHER_JNI(android_util_MemoryIntArray_open)},
        {"nativeClose", "(IJZ)V", GESHER_JNI(android_util_MemoryIntArray_close)},
        {"nativeGet", "(IJI)I", GESHER_JNI(android_util_MemoryIntArray_get)},
        {"nativeSet", "(IJII)V", GESHER_JNI(android_util_MemoryIntArray_set)},
        {"nativeSize", "(I)I", GESHER_JNI(android_util_MemoryIntArray_size)},
    };
    return jniRegisterNativeMethods(env, "android/util/MemoryIntArray", kMethods, NELEM(kMethods));
}
}  // namespace android
