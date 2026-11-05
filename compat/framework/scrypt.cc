// android.security.Scrypt uses the already built BoringSSL RFC 7914 primitive.
// Its Java contract returns null for absent inputs or derivation failure. JNI
// allocation failures retain their pending exception and never dereference a
// null array pointer (including while acquiring the second or output array).
#include <jni.h>
#include <limits>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/ScopedPrimitiveArray.h>
#include <openssl/evp.h>

namespace {
jbyteArray JNICALL NativeScrypt(JNIEnv* env, jobject, jbyteArray password, jbyteArray salt,
                               jint n, jint r, jint p, jint outputLength) {
    if (password == nullptr || salt == nullptr) return nullptr;
    // NewByteArray's JNI precondition excludes negative lengths. CheckJNI
    // aborts the process rather than raising a Java exception for a violation.
    if (outputLength < 0) {
        jniThrowException(env, "java/lang/NegativeArraySizeException", nullptr);
        return nullptr;
    }
    jbyteArray output = env->NewByteArray(outputLength);
    if (output == nullptr) return nullptr;
    // Reject signed values before converting to the unsigned crypto API.
    if (n < 2 || r <= 0 || p <= 0) return nullptr;
    ScopedByteArrayRO passwordBytes(env, password);
    if (passwordBytes.get() == nullptr) return nullptr;
    ScopedByteArrayRO saltBytes(env, salt);
    if (saltBytes.get() == nullptr) return nullptr;
    ScopedByteArrayRW outputBytes(env, output);
    if (outputBytes.get() == nullptr) return nullptr;
    // AOSP's crypto_scrypt has no additional memory ceiling. Preserve that
    // behavior rather than inheriting BoringSSL's default memory limit.
    int ok = EVP_PBE_scrypt(reinterpret_cast<const char*>(passwordBytes.get()),
                           passwordBytes.size(), reinterpret_cast<const uint8_t*>(saltBytes.get()),
                           saltBytes.size(), n, r, p, std::numeric_limits<size_t>::max(),
                           reinterpret_cast<uint8_t*>(outputBytes.get()), outputLength);
    return ok == 1 ? output : nullptr;
}
}  // namespace

namespace android {
int register_gesher_Scrypt(JNIEnv* env) {
    static const JNINativeMethod kMethods[] = {
        {"nativeScrypt", "([B[BIIII)[B", reinterpret_cast<void*>(&NativeScrypt)},
    };
    return jniRegisterNativeMethods(env, "android/security/Scrypt", kMethods,
                                    sizeof(kMethods) / sizeof(kMethods[0]));
}
}  // namespace android
