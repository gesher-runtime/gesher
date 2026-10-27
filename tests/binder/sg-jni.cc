// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <jni.h>
#include "sg-actor.h"

namespace {
bool CopyString(JNIEnv* env, jstring input, std::string* output) {
  if (!input) return false;
  const char* text = env->GetStringUTFChars(input, nullptr);
  if (!text) return false;
  *output = text;
  env->ReleaseStringUTFChars(input, text);
  return true;
}
jint Run(JNIEnv* env, jclass, jstring role, jstring scenario, jstring prefix, jstring endpoint) {
  std::string values[4];
  if (!CopyString(env, role, &values[0]) || !CopyString(env, scenario, &values[1]) ||
      !CopyString(env, prefix, &values[2]) || !CopyString(env, endpoint, &values[3]) || values[3].empty()) return 2;
  const char* previous = binder_sg_fixture::selected_device;
  binder_sg_fixture::selected_device = values[3].c_str();
  char name[] = "BinderSgActor";
  char* arguments[]{name, values[0].data(), values[1].data(), values[2].data()};
  const int result = binder_sg_fixture::Main(4, arguments);
  binder_sg_fixture::selected_device = previous;
  return result;
}
jstring CurrentEndpoint(JNIEnv* env, jclass) {
  const char* endpoint = getenv("GESHER_BINDER");
  return endpoint ? env->NewStringUTF(endpoint) : nullptr;
}
}  // namespace
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  const JNINativeMethod methods[]{
      {"run", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I", reinterpret_cast<void*>(&Run)},
      {"currentEndpoint", "()Ljava/lang/String;", reinterpret_cast<void*>(&CurrentEndpoint)}};
  bool registered = false;
  for (const char* name : {"gesher/BinderSgActor", "com/gesher/bindersg/SgActivity"}) {
    jclass type = env->FindClass(name);
    if (!type) { env->ExceptionClear(); continue; }
    registered |= env->RegisterNatives(type, methods, 2) == JNI_OK;
    env->DeleteLocalRef(type);
  }
  return registered ? JNI_VERSION_1_6 : JNI_ERR;
}
