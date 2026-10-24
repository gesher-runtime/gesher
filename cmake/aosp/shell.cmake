# Gesher: Windows shell integration of installed Android apps (shell track,
# docs/installer.md): com.gesher.app.shell.Shell's natives, in
# libandroid_runtime's builtin library (android_runtime.cc registers them).
# shell_win32.cc uses COM (ole32, uuid) and finds shell32's functions at run
# time.
target_sources(framework_runtime PRIVATE
  ${CMAKE_SOURCE_DIR}/compat/shell/shell_natives.cc
  ${CMAKE_SOURCE_DIR}/compat/shell/shell_win32.cc)
target_include_directories(framework_runtime PRIVATE ${CMAKE_SOURCE_DIR}/compat/shell)
target_link_libraries(framework_runtime PUBLIC ole32 uuid)
