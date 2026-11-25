// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// eglDestroyContext on one thread while another thread keeps drawing with its
// own context of Android's default display (issue #485): the scenarios of
// egl_context_destroy_scenarios.h on ANGLE's Microsoft-ABI entry points
// (libEGL.dll/libGLESv2.dll next to the executable) and Gesher's default
// display, as the app's GL threads share it. Run by track 40-native-libs on
// the default adapter and on WARP (GESHER_EGL_DEVICE=warp). Prints
// "egl_context_destroy_test all passed" and exits 0 when every check holds;
// with an ANGLE without compat/angle/patches/0001 it dies with an access
// violation in libGLESv2.dll at "thread A draws after X was destroyed".
#include "egl_context_destroy_scenarios.h"

#include <gesher/egl_angle.h>

#include <cstdio>

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  egl_context_destroy::Api api;
  bool complete = true;
#define EGL_CONTEXT_DESTROY_LOAD(name)                                          \
  api.name = reinterpret_cast<decltype(api.name)>(gesher_egl_proc(#name)); \
  if (api.name == nullptr) {                                                    \
    std::printf("FAILED: ANGLE has no %s\n", #name);                            \
    complete = false;                                                           \
  }
  EGL_CONTEXT_DESTROY_FUNCTIONS(EGL_CONTEXT_DESTROY_LOAD)
#undef EGL_CONTEXT_DESTROY_LOAD
  if (!complete) {
    std::printf("egl_context_destroy_test FAILED\n");
    return 1;
  }
  return egl_context_destroy::RunAll(api, gesher_egl_default_display(), "egl_context_destroy_test");
}
