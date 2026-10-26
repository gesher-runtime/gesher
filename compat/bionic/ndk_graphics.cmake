# gesher: the graphics half of the NDK for the app's .so files
# (docs/native-libs.md, "Graphics"; ADR 0023): libandroid's ANativeWindow_*
# (libandroid_window.cc, over dalvikvm's Microsoft-ABI functions in
# compat/hwui/window) and EGL/GLES (egl_gles.cc, over ANGLE's libEGL.dll and
# libGLESv2.dll, loaded at first use). An OBJECT library: static initializers
# add the System V entries to the bionic shim's table. The thunks for every
# EGL/GLES function are generated from AOSP's Khronos headers
# (frameworks/native/opengl/include) by gen_gl_thunks.py.
set(_gl_include ${AOSP_DIR}/frameworks/native/opengl/include)
set(_gl_gen ${CMAKE_BINARY_DIR}/gen/bionic)
# Written by hand in egl_gles.cc (displays, window surfaces, configs,
# callbacks, eglGetProcAddress).
set(_gl_handwritten eglGetDisplay eglGetPlatformDisplay eglGetPlatformDisplayEXT
  eglCreateWindowSurface eglCreatePlatformWindowSurface eglCreatePlatformWindowSurfaceEXT
  eglDestroySurface eglChooseConfig eglGetConfigAttrib eglGetProcAddress
  eglDebugMessageControlKHR eglSetBlobCacheFuncsANDROID glDebugMessageCallback
  glDebugMessageCallbackKHR
  # surfaces track (egl_child_surface.cc): a child surface's frame is posted before the swap.
  eglSwapBuffers eglSwapBuffersWithDamageKHR eglSwapBuffersWithDamageEXT)
set(_gl_headers)
foreach(h EGL/egl.h EGL/eglext.h GLES3/gl32.h GLES3/gl31.h GLES3/gl3.h GLES2/gl2.h
    GLES2/gl2ext.h GLES/gl.h GLES/glext.h)
  list(APPEND _gl_headers ${_gl_include}/${h})
endforeach()
add_custom_command(OUTPUT ${_gl_gen}/gl_thunks.inc
  COMMAND ${CMAKE_COMMAND} -E make_directory ${_gl_gen}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/gen_gl_thunks.py
    ${_gl_gen}/gl_thunks.inc ${_gl_include} ${_gl_handwritten}
  DEPENDS ${CMAKE_CURRENT_LIST_DIR}/gen_gl_thunks.py ${_gl_headers}
  COMMENT "Generating the EGL/GLES System V thunks"
  VERBATIM)

add_library(gesher_ndk_graphics OBJECT
  ${CMAKE_CURRENT_LIST_DIR}/libandroid_window.cc
  ${CMAKE_CURRENT_LIST_DIR}/egl_gles.cc
  ${CMAKE_CURRENT_LIST_DIR}/egl_child_surface.cc
  ${_gl_gen}/gl_thunks.inc)
target_include_directories(gesher_ndk_graphics PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${_gl_gen}
  ${_gl_include} ${CMAKE_SOURCE_DIR}/compat/hwui/window)
# graphics_native_headers (cmake/aosp/hwui.cmake): <android/native_window.h>.
target_link_libraries(gesher_ndk_graphics PRIVATE gesher_bionic gesher_elf graphics_native_headers
  user32)
target_compile_options(gesher_ndk_graphics PRIVATE -Wall -Wextra -Werror)
target_link_libraries(dalvikvm PRIVATE gesher_ndk_graphics)

# The NDK library drawing into Gesher windows without Java
# (tests/native/gl_window_test.cc), run in CI on Windows with libglapp.so.
add_executable(gl_window_test ${CMAKE_SOURCE_DIR}/tests/native/gl_window_test.cc)
target_include_directories(gl_window_test PRIVATE ${CMAKE_SOURCE_DIR}/compat/hwui/window)
target_link_libraries(gl_window_test PRIVATE gesher_ndk_graphics gesher_bionic gesher_elf
  gesher_window_surface graphics_native_headers user32 gdi32)
target_compile_options(gl_window_test PRIVATE -Wall -Wextra -Werror)
