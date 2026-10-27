#include <jni.h>
#include "net4_resolver.h"
namespace {
jstring Run(JNIEnv* env, jclass, jboolean allowed) {
  return env->NewStringUTF(net4resolver::Run(allowed == JNI_TRUE).c_str());
}
}
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  const JNINativeMethod method = {"run", "(Z)Ljava/lang/String;", reinterpret_cast<void*>(&Run)};
  bool registered = false;
  for (const char* name : {"gesher/Net4ResolverTest", "com/gesher/net4resolver/ResolverActivity"}) {
    jclass cls = env->FindClass(name);
    if (cls == nullptr) { env->ExceptionClear(); continue; }
    registered |= env->RegisterNatives(cls, &method, 1) == JNI_OK;
    env->DeleteLocalRef(cls);
  }
  return registered ? JNI_VERSION_1_6 : JNI_ERR;
}
