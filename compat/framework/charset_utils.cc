// Framework fast string buffers use ART's JNI UTF conversion, including its
// established four-byte supplementary encoding. Keep AOSP's decoder and its
// temporary terminator restoration; use overflow-safe capacity arithmetic for
// the encoder instead of the upstream signed srcLen*4 fast-path calculation.
#include <limits.h>
#include <stdint.h>
#include "android_util_CharsetUtils.cpp"

namespace android {
namespace {
jint ToUtf8(JNIEnv* env, jobject, jstring source, jint sourceLength, jlong destination,
            jint offset, jint capacity) {
    if (source == nullptr) {
        jniThrowNullPointerException(env, "source");
        return 0;
    }
    // The Java entry point supplies String.length(), rather than a substring.
    if (sourceLength != env->GetStringLength(source)) {
        jniThrowException(env, "java/lang/StringIndexOutOfBoundsException", nullptr);
        return 0;
    }
    jint encoded = env->GetStringUTFLength(source);
    // The terminating NUL must fit as well; failed sizing leaves storage alone.
    if (offset < 0 || offset >= capacity || encoded >= capacity - offset) return -encoded;
    uintptr_t base = static_cast<uintptr_t>(destination);
    if (base == 0) {
        jniThrowNullPointerException(env, "destination");
        return 0;
    }
    if (base > UINTPTR_MAX - static_cast<uintptr_t>(offset) - static_cast<uintptr_t>(encoded)) {
        jniThrowException(env, "java/lang/IndexOutOfBoundsException", nullptr);
        return 0;
    }
    env->GetStringUTFRegion(source, 0, sourceLength, reinterpret_cast<char*>(base) + offset);
    return encoded;
}

jstring FromUtf8(JNIEnv* env, jobject clazz, jlong source, jint offset, jint length) {
    if (offset < 0 || length < 0 || offset > INT_MAX - length ||
        static_cast<uintptr_t>(source) > UINTPTR_MAX - static_cast<uintptr_t>(offset) -
                                         static_cast<uintptr_t>(length)) {
        jniThrowException(env, "java/lang/IndexOutOfBoundsException", nullptr);
        return nullptr;
    }
    if (source == 0) {
        jniThrowNullPointerException(env, "source");
        return nullptr;
    }
    return android_util_CharsetUtils_fromModifiedUtf8Bytes(env, clazz, source, offset, length);
}
}  // namespace

int register_gesher_CharsetUtils(JNIEnv* env) {
    static const JNINativeMethod kMethods[] = {
        {"toModifiedUtf8Bytes", "(Ljava/lang/String;IJII)I", GESHER_JNI(ToUtf8)},
        {"fromModifiedUtf8Bytes", "(JII)Ljava/lang/String;", GESHER_JNI(FromUtf8)},
    };
    return jniRegisterNativeMethods(env, "android/util/CharsetUtils", kMethods, NELEM(kMethods));
}
}  // namespace android
