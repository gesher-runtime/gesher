# Gesher: what the system server's device-state services ask of Windows
# (devsvc track, docs/services.md "Device services", ADR 0038):
# com.gesher.server.WindowsHost's natives, in libandroid_runtime's builtin
# library (android_runtime.cc registers them). Toasts are WinRT, reached through
# combase's functions found at run time (devsvc_toast.c, C: mingw-w64's WinRT
# headers have a usable C form only); the clipboard, power requests and the
# event window are user32/kernel32; the network list manager is COM.
target_sources(framework_runtime PRIVATE
  ${CMAKE_SOURCE_DIR}/compat/devsvc/devsvc_natives.cc
  ${CMAKE_SOURCE_DIR}/compat/devsvc/devsvc_win32.cc
  ${CMAKE_SOURCE_DIR}/compat/devsvc/devsvc_toast.c
  ${CMAKE_SOURCE_DIR}/compat/devsvc/devsvc_activator.c)
target_include_directories(framework_runtime PRIVATE ${CMAKE_SOURCE_DIR}/compat/devsvc)
target_link_libraries(framework_runtime PUBLIC ole32 oleaut32 uuid user32 advapi32)
