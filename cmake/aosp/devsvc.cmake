# Gesher: what the system server's device-state services ask of Windows
# (devsvc track, docs/services.md "Device services", ADR 0038):
# com.gesher.server.WindowsHost's natives, in libandroid_runtime's builtin
# library (android_runtime.cc registers them). Toasts are WinRT, reached through
# combase's functions found at run time (devsvc_toast.c, C: mingw-w64's WinRT
# headers have a usable C form only); the clipboard, power requests and the
# event window are user32/kernel32; the network list manager is COM. The
# hardware behind system features (devsvc_hardware.cc) is user32, MMDevice and
# iphlpapi.
target_sources(framework_runtime PRIVATE
  ${CMAKE_SOURCE_DIR}/compat/devsvc/devsvc_natives.cc
  ${CMAKE_SOURCE_DIR}/compat/devsvc/devsvc_win32.cc
  ${CMAKE_SOURCE_DIR}/compat/devsvc/camera_image_framing_win.cc
  ${CMAKE_SOURCE_DIR}/compat/devsvc/url_handlers_win.cc
  ${CMAKE_SOURCE_DIR}/compat/devsvc/devsvc_hardware.cc
  ${CMAKE_SOURCE_DIR}/compat/devsvc/home_region_win.cc
  ${CMAKE_SOURCE_DIR}/compat/devsvc/clipboard_access.cc
  ${CMAKE_SOURCE_DIR}/compat/devsvc/devsvc_toast.c
  ${CMAKE_SOURCE_DIR}/compat/devsvc/devsvc_activator.c)
# Release pinned JNI strings and report an unavailable clipboard write when
# allocating its immutable input fails; other framework sources keep AOSP flags.
set_property(SOURCE ${CMAKE_SOURCE_DIR}/compat/devsvc/devsvc_natives.cc
  APPEND PROPERTY COMPILE_OPTIONS -fexceptions)
# camera_image_framing_win.cc draws a GDI preview and uses host_dialog_theme.h.
set_property(SOURCE ${CMAKE_SOURCE_DIR}/compat/devsvc/camera_image_framing_win.cc
  APPEND PROPERTY COMPILE_OPTIONS -UNOGDI)
target_include_directories(framework_runtime PRIVATE ${CMAKE_SOURCE_DIR}/compat/devsvc)
# gesher/egl_angle.h: the OpenGL ES version of the default EGL display (egl_gles.cc).
set_property(SOURCE ${CMAKE_SOURCE_DIR}/compat/devsvc/devsvc_natives.cc
  APPEND PROPERTY INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/compat/bionic/include)
target_link_libraries(framework_runtime PUBLIC ole32 oleaut32 uuid user32 gdi32 comctl32
  windowscodecs advapi32 shell32 shlwapi wtsapi32 iphlpapi)
