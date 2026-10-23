// Gesher: android.app.PropertyInvalidatedCache's natives, frameworks/base's own
// core/jni source included here so that its functions can be registered
// through GESHER_JNI (System V thunks) without another JNI table patch, as
// services_natives.cc does for ApplicationSharedMemory (a translation unit of
// its own: both sources name their table gMethods). The nonces live in the
// ApplicationSharedMemory region the system server shares with each app
// process (services track, docs/services.md).

// First: it defines LOG_TAG, which headers included before it would define too.
#include "android_app_PropertyInvalidatedCache.cpp"

namespace android {

static const JNINativeMethod gGesherPropertyInvalidatedCacheMethods[] = {
    {"nativeGetMaxNonce", "(J)I", GESHER_JNI(getMaxNonce)},
    {"nativeGetMaxByte", "(J)I", GESHER_JNI(getMaxByte)},
    {"nativeSetByteBlock", "(JI[B)V", GESHER_JNI(setByteBlock)},
    {"nativeGetByteBlock", "(JI[B)I", GESHER_JNI(getByteBlock)},
    {"nativeGetByteBlockHash", "(J)I", GESHER_JNI(getByteBlockHash)},
    {"nativeGetNonce", "(JI)J", GESHER_JNI(getNonce)},
    {"nativeSetNonce", "(JIJ)Z", GESHER_JNI(setNonce)},
};

int register_gesher_PropertyInvalidatedCache(JNIEnv* env) {
    return RegisterMethodsOrDie(env, "android/app/PropertyInvalidatedCache",
                                gGesherPropertyInvalidatedCacheMethods,
                                NELEM(gGesherPropertyInvalidatedCacheMethods));
}

}  // namespace android
