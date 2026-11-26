// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// The scenarios of tests/native/egl_context_destroy_scenarios.h (issue #485)
// in an app's JNI library: two pthreads of the guest process, the NDK's
// libEGL/libGLESv2 imports and EGL_DEFAULT_DISPLAY, so each GL call goes
// through Gesher's EGL/GLES thunks (compat/bionic/egl_gles.cc) and, for
// arm64-v8a, through the translator, to the one ANGLE display. Built for
// x86_64 and arm64-v8a by .github/workflows/native2.yml as
// libeglcontextdestroy.so; gesher.EglContextDestroyTest runs it in dalvikvm
// (track 44-native2). run() returns the checks' lines, the last one
// "egl_context_destroy all passed" or "egl_context_destroy FAILED"; the
// progress lines also go to stderr, so a crash shows where it happened.
#include <jni.h>

#include "../native/egl_context_destroy_scenarios.h"

namespace {

jstring Run(JNIEnv* env, jclass) {
  egl_context_destroy::Api api;
#define EGL_CONTEXT_DESTROY_LOAD(name) api.name = &::name;
  EGL_CONTEXT_DESTROY_FUNCTIONS(EGL_CONTEXT_DESTROY_LOAD)
#undef EGL_CONTEXT_DESTROY_LOAD
  std::string transcript;
  EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr) == EGL_TRUE) {
    egl_context_destroy::RunAll(api, display, "egl_context_destroy", &transcript);
  } else {
    char line[128];
    std::snprintf(line, sizeof(line),
                  "egl_context_destroy: FAILED: eglInitialize(EGL_DEFAULT_DISPLAY): 0x%x\n"
                  "egl_context_destroy FAILED\n", eglGetError());
    transcript = line;
  }
  return env->NewStringUTF(transcript.c_str());
}

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass cls = env->FindClass("gesher/EglContextDestroyTest");
  if (cls == nullptr) return JNI_ERR;
  const JNINativeMethod method = {"run", "()Ljava/lang/String;", reinterpret_cast<void*>(&Run)};
  bool registered = env->RegisterNatives(cls, &method, 1) == JNI_OK;
  env->DeleteLocalRef(cls);
  return registered ? JNI_VERSION_1_6 : JNI_ERR;
}
