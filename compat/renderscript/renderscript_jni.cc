// Gesher: librs_jni.so, the library android.renderscript.RenderScript loads
// (System.loadLibrary("rs_jni") in its static initializer), as a builtin
// library (docs/renderscript.md). Its JNI_OnLoad is AOSP's
// (frameworks/base rs/jni/android_renderscript_RenderScript.cpp, copied by
// gen_jni_source.py), which registers RenderScript's natives over libRS's
// CPU runtime (cmake/aosp/renderscript.cmake).

#include <jni.h>

#include <gesher/builtin_library.h>

jint gesher_rs_jni_OnLoad(JavaVM* vm, void* reserved);

extern "C" jint JNI_OnLoad_rs_jni(JavaVM* vm, void* reserved) {
  return gesher_rs_jni_OnLoad(vm, reserved);
}

namespace {

const gesher_builtin_symbol kRsJni[] = {
    {"JNI_OnLoad", reinterpret_cast<void*>(&JNI_OnLoad_rs_jni)},
};

[[maybe_unused]] const bool registered = [] {
  gesher_register_builtin_library("librs_jni.so", kRsJni, sizeof(kRsJni) / sizeof(kRsJni[0]));
  return true;
}();

}  // namespace
