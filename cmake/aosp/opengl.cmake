# The framework's Java OpenGL ES and EGL bindings over ANGLE (docs/angle.md,
# "Java apps"): libandroid_runtime's com_google_android_gles_jni_* (EGL10,
# GL10 and its extensions), android_opengl_* (EGL14/15, EGLExt, GLES10-32) and
# android/opengl/util.cpp (GLUtils, ETC1, Visibility), with libETC1
# (frameworks/native/opengl/libs/ETC1). Their JNI tables register System V
# entries (frameworks/base patch 0760, tools/jni-sysv-patch.py gl).
#
# They call eglXxx/glXxx as code linked against Android's libEGL and
# libGLESv*: compat/hwui/gl/angle_gl.cc and compat/opengl/opengl_jni.cc define
# those with Android's meanings, gen_gl_forwarders.py every other one of the
# Khronos headers, each forwarding to ANGLE's entry point resolved on first
# call (gesher_egl_proc). Compiled with hwui's EGL/eglplatform.h
# (EGLNativeWindowType is ANativeWindow*, no dllimport); libgui's GLConsumer and
# libEGL's private display.h are compat/opengl/include's.

set(_ogl_include ${AOSP_DIR}/frameworks/native/opengl/include)
set(_ogl_gen ${CMAKE_BINARY_DIR}/gen/opengl)
# Defined by hand: the generator leaves their names out.
set(_ogl_handwritten
  ${CMAKE_SOURCE_DIR}/compat/hwui/gl/angle_gl.cc
  ${CMAKE_SOURCE_DIR}/compat/opengl/opengl_jni.cc)
set(_ogl_headers)
foreach(h EGL/egl.h EGL/eglext.h GLES3/gl32.h GLES3/gl31.h GLES3/gl3.h GLES2/gl2.h
    GLES2/gl2ext.h GLES/gl.h GLES/glext.h)
  list(APPEND _ogl_headers ${_ogl_include}/${h})
endforeach()
add_custom_command(OUTPUT ${_ogl_gen}/gl_forwarders.cc
  COMMAND ${CMAKE_COMMAND} -E make_directory ${_ogl_gen}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/compat/opengl/gen_gl_forwarders.py
    ${_ogl_gen}/gl_forwarders.cc ${_ogl_include} ${_ogl_handwritten}
  DEPENDS ${CMAKE_SOURCE_DIR}/compat/opengl/gen_gl_forwarders.py
    ${CMAKE_SOURCE_DIR}/compat/bionic/gen_gl_thunks.py ${_ogl_headers} ${_ogl_handwritten}
  COMMENT "Generating the EGL/GLES forwarders to ANGLE"
  VERBATIM)

add_library(gesher_opengl OBJECT
  ${_fwjni}/android/opengl/poly_clip.cpp
  ${_fwjni}/android/opengl/util.cpp
  ${_fwjni}/android_opengl_EGL14.cpp
  ${_fwjni}/android_opengl_EGL15.cpp
  ${_fwjni}/android_opengl_EGLExt.cpp
  ${_fwjni}/android_opengl_GLES10.cpp
  ${_fwjni}/android_opengl_GLES10Ext.cpp
  ${_fwjni}/android_opengl_GLES11.cpp
  ${_fwjni}/android_opengl_GLES11Ext.cpp
  ${_fwjni}/android_opengl_GLES20.cpp
  ${_fwjni}/android_opengl_GLES30.cpp
  ${_fwjni}/android_opengl_GLES31.cpp
  ${_fwjni}/android_opengl_GLES31Ext.cpp
  ${_fwjni}/android_opengl_GLES32.cpp
  ${_fwjni}/com_google_android_gles_jni_EGLImpl.cpp
  ${_fwjni}/com_google_android_gles_jni_GLImpl.cpp
  ${AOSP_DIR}/frameworks/native/opengl/libs/ETC1/etc1.cpp
  ${CMAKE_SOURCE_DIR}/compat/opengl/opengl_jni.cc
  ${_ogl_gen}/gl_forwarders.cc)
target_include_directories(gesher_opengl BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/compat/hwui/gl/include  # EGL/eglplatform.h
  ${CMAKE_SOURCE_DIR}/compat/opengl/include)
target_include_directories(gesher_opengl PRIVATE
  ${CMAKE_SOURCE_DIR}/compat/opengl
  ${CMAKE_SOURCE_DIR}/compat/bionic/include  # gesher/egl_angle.h
  ${CMAKE_SOURCE_DIR}/compat/framework/include
  ${_ogl_include}
  ${_fwjni} ${_fwjni}/include
  ${_fw}/libs/hwui/apex/include)  # android/graphics/bitmap.h (util.cpp)
# The extensions' prototypes, as Android.bp's libandroid_runtime declares them.
target_compile_definitions(gesher_opengl PRIVATE GL_APICALL= GL_API= EGL_EGLEXT_PROTOTYPES
  GL_GLEXT_PROTOTYPES)
target_link_libraries(gesher_opengl PRIVATE hostgraphics ui_types graphics_native_headers
  nativehelper utils cutils log base)
gesher_graphics_cxx(gesher_opengl)
# The generated sources (glgen) leave parameters and variables unused and
# compare signed and unsigned; EGLImpl's config arrays are variable-length.
target_compile_options(gesher_opengl PRIVATE -Wall -Werror -Wno-unused-parameter
  -Wno-unused-variable -Wno-unused-function -Wno-sign-compare -Wno-unused-but-set-variable
  -Wno-vla-cxx-extension)

target_link_libraries(dalvikvm PRIVATE gesher_opengl)
