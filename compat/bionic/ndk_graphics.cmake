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
  eglSwapBuffers eglSwapBuffersWithDamageKHR eglSwapBuffersWithDamageEXT
  # ADR 0089 (egl_native_buffer.cc): AHardwareBuffer images, native fences, the extensions.
  eglGetNativeClientBufferANDROID eglCreateImageKHR eglCreateImage eglDestroyImageKHR
  eglDestroyImage eglCreateSyncKHR eglCreateSync eglDestroySyncKHR eglDestroySync
  eglClientWaitSyncKHR eglClientWaitSync eglWaitSyncKHR eglWaitSync eglGetSyncAttribKHR
  eglGetSyncAttrib eglSignalSyncKHR eglDupNativeFenceFDANDROID eglQueryString)
set(_gl_headers)
foreach(h EGL/egl.h EGL/eglext.h GLES3/gl32.h GLES3/gl31.h GLES3/gl3.h GLES2/gl2.h
    GLES2/gl2ext.h GLES/gl.h GLES/glext.h)
  list(APPEND _gl_headers ${_gl_include}/${h})
endforeach()
# ANGLE's own extensions (GL_ANGLE_*, GL_CHROMIUM_*), at the DLLs' commit.
list(APPEND _gl_headers ${CMAKE_CURRENT_LIST_DIR}/third_party/angle/GLES2/gl2ext_angle.h)
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
  # ADR 0089: AHardwareBuffer, ASurfaceControl, their EGL half, the GPU thread.
  ${CMAKE_CURRENT_LIST_DIR}/gpu_interop.cc
  ${CMAKE_CURRENT_LIST_DIR}/gpu_fence.cc
  ${CMAKE_CURRENT_LIST_DIR}/hardware_buffer.cc
  ${CMAKE_CURRENT_LIST_DIR}/surface_control.cc
  ${CMAKE_CURRENT_LIST_DIR}/egl_native_buffer.cc
  # The GPU kept from sleeping between frames read back (child surfaces, composition).
  ${CMAKE_CURRENT_LIST_DIR}/gpu_keepalive.cc
  ${_gl_gen}/gl_thunks.inc)
target_include_directories(gesher_ndk_graphics PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${_gl_gen}
  ${_gl_include} ${CMAKE_SOURCE_DIR}/compat/hwui/window)
# graphics_native_headers (cmake/aosp/hwui.cmake): <android/native_window.h>.
# d3d11: the GPU thread's own device (gpu_interop.cc) and the keep-alive's (gpu_keepalive.cc).
target_link_libraries(gesher_ndk_graphics PRIVATE gesher_bionic gesher_elf graphics_native_headers
  user32 d3d11)
target_compile_options(gesher_ndk_graphics PRIVATE -Wall -Wextra -Werror)
target_link_libraries(dalvikvm PRIVATE gesher_ndk_graphics)

# The NDK library drawing into Gesher windows without Java
# (tests/native/gl_window_test.cc), run in CI on Windows with libglapp.so.
add_executable(gl_window_test ${CMAKE_SOURCE_DIR}/tests/native/gl_window_test.cc)
target_include_directories(gl_window_test PRIVATE ${CMAKE_SOURCE_DIR}/compat/hwui/window)
target_link_libraries(gl_window_test PRIVATE gesher_ndk_graphics gesher_bionic gesher_elf
  gesher_window_surface graphics_native_headers user32 gdi32)
target_compile_options(gl_window_test PRIVATE -Wall -Wextra -Werror)

# AHardwareBuffer, ASurfaceControl and the EGL native buffer/fence extensions
# from an NDK library into a Gesher window (tests/native/surface_control_test.c,
# ADR 0089), run in CI on Windows with surface_control_test.so.
add_executable(surface_control_host_test ${CMAKE_SOURCE_DIR}/tests/native/surface_control_host_test.cc)
target_include_directories(surface_control_host_test PRIVATE ${CMAKE_SOURCE_DIR}/compat/hwui/window)
target_link_libraries(surface_control_host_test PRIVATE gesher_ndk_graphics gesher_bionic gesher_elf
  gesher_window_surface graphics_native_headers user32 gdi32)
target_compile_options(surface_control_host_test PRIVATE -Wall -Wextra -Werror)

# The GPU kept from sleeping between frames read back
# (tests/native/gpu_keepalive_test.cc): on a hardware adapter, off on WARP.
add_executable(gpu_keepalive_test ${CMAKE_SOURCE_DIR}/tests/native/gpu_keepalive_test.cc)
target_include_directories(gpu_keepalive_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(gpu_keepalive_test PRIVATE gesher_ndk_graphics gesher_bionic gesher_elf
  gesher_window_surface graphics_native_headers user32 gdi32 winmm)
target_compile_options(gpu_keepalive_test PRIVATE -Wall -Wextra -Werror)

# Real GPU fences, nonblocking server waits, context concurrency, pending
# destruction, fd aliases and cross-process ownership (also on WARP).
add_executable(egl_fence_test ${CMAKE_SOURCE_DIR}/tests/native/egl_fence_test.cc)
target_include_directories(egl_fence_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_SOURCE_DIR}/compat/posix/src ${_gl_include})
target_link_libraries(egl_fence_test PRIVATE gesher_ndk_graphics gesher_bionic gesher_elf
  gesher_window_surface gesher_exec_spawn graphics_native_headers user32 gdi32 d3d11)
target_compile_options(egl_fence_test PRIVATE -Wall -Wextra -Werror)

# A GL test library's gesher_selftest() with the same EGL/GLES
# (tests/native/gl_extension_test.cc; gl_extension_procs_test.so in CI).
add_executable(gl_extension_test ${CMAKE_SOURCE_DIR}/tests/native/gl_extension_test.cc)
target_link_libraries(gl_extension_test PRIVATE gesher_ndk_graphics gesher_bionic gesher_elf
  gesher_window_surface graphics_native_headers user32 gdi32)
target_compile_options(gl_extension_test PRIVATE -Wall -Wextra -Werror)
