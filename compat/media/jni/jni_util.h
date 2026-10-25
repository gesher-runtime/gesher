// Gesher media: JNI helpers for the media natives (docs/media.md).
#pragma once

#include <jni.h>

#include <string>

namespace gesher::media {

// The VM, set by the first registration.
JavaVM* Vm();
void SetVm(JNIEnv* env);

// This thread's JNIEnv, attaching it (as a daemon named `name`) if needed.
JNIEnv* AttachedEnv(const char* name);

// Every native method `className` declares gets a stand-in that answers zero,
// false or null (found by reflection, so the list follows framework.jar);
// the methods Gesher implements are registered over them afterwards. Returns
// false when the class is not on the class path.
bool RegisterStandins(JNIEnv* env, const char* className);

// Registers `methods` one by one: a method the class lacks is logged and
// skipped, not fatal. Returns the number registered.
int RegisterMethods(JNIEnv* env, const char* className, const JNINativeMethod* methods, int count);

template <size_t N>
int RegisterMethods(JNIEnv* env, const char* className, const JNINativeMethod (&methods)[N]) {
  return RegisterMethods(env, className, methods, static_cast<int>(N));
}

// Java strings.
std::string Utf8(JNIEnv* env, jstring s);
std::u16string Utf16(JNIEnv* env, jstring s);

// Clears and logs a pending exception; true if there was one.
bool CheckException(JNIEnv* env, const char* where);

}  // namespace gesher::media

// The natives are System V functions (JNICALL under GESHER, as ART calls them;
// docs/decisions/0006-managed-abi.md).
#define GESHER_NATIVE(fn) reinterpret_cast<void*>(&(fn))
