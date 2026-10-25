// Gesher: Conscrypt's native library, libjavacrypto, linked into dalvikvm.
//
// Conscrypt's NativeCryptoJni loads it with System.loadLibrary("javacrypto");
// it is registered here as a built-in library of gesher-base's dlopen
// (<gesher/builtin_library.h>), like libcore's (ADR 0011). Its JNI_OnLoad is
// Conscrypt's own, compiled as JNI_OnLoad_conscrypt (STATIC_LIB), after the
// system CA certificates are brought up to date from Windows' store
// (trust_store.cc): every TLS client loads this library before it looks a
// certificate up there.

#include <gesher/builtin_library.h>

#include <jni.h>

#include "trust_store.h"

jint JNI_OnLoad_conscrypt(JavaVM* vm, void* reserved);  // jniload.cc

namespace {

jint OnLoad(JavaVM* vm, void* reserved) {
  gesher::net::SyncSystemCaCerts();
  return JNI_OnLoad_conscrypt(vm, reserved);
}

const gesher_builtin_symbol kJavacrypto[] = {
    {"JNI_OnLoad", reinterpret_cast<void*>(&OnLoad)},
};

const bool registered = [] {
  gesher_register_builtin_library("libjavacrypto.so", kJavacrypto,
                                  sizeof(kJavacrypto) / sizeof(kJavacrypto[0]));
  return true;
}();

}  // namespace
