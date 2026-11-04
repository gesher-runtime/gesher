// Gesher: libEGL's private header (frameworks/native/opengl/libs/EGL), for
// com_google_android_gles_jni_EGLImpl.cpp; compat/opengl/opengl_jni.cc
// defines what it declares.
#pragma once

#include <EGL/egl.h>

namespace android {

EGLint egl_get_init_count(EGLDisplay dpy);

}  // namespace android
