// libnativeapp.so, the native code of tests/apps/nativeapp: C++ against the
// NDK's libc++_shared.so, which the APK carries next to it in lib/x86_64.

#include <jni.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace {

// A C++ exception thrown by libc++ and caught here.
std::string ExceptionCheck() {
  try {
    std::vector<int> v(2);
    return "no exception " + std::to_string(v.at(5));
  } catch (const std::out_of_range&) {
    return "caught std::out_of_range";
  }
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_gesher_nativeapp_NativeActivity_stringFromJNI(JNIEnv* env, jclass) {
  std::string s = "Hello from C++, " + ExceptionCheck();
  return env->NewStringUTF(s.c_str());
}

// Calls NativeActivity.square(x) + 1 through the C++ JNIEnv wrapper (the
// va_list form of CallIntMethod).
extern "C" JNIEXPORT jint JNICALL
Java_com_gesher_nativeapp_NativeActivity_callBack(JNIEnv* env, jobject self, jint x) {
  jclass cls = env->GetObjectClass(self);
  jmethodID square = env->GetMethodID(cls, "square", "(I)I");
  return env->CallIntMethod(self, square, x) + 1;
}
