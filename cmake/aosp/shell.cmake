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

# gesher-launch.exe: what an app's shortcut, a double-clicked .apk and a toast
# click start (Windows subsystem: no console window flashes; it hands on the
# foreground to the app's window). gesher_shellext.dll: the .apk file type's
# icon handler, loaded by Explorer (linked statically, as everything here).
add_executable(gesher-launch WIN32 ${CMAKE_SOURCE_DIR}/compat/shell/gesher_launch.cc)
target_compile_options(gesher-launch PRIVATE -Wall -Wextra -Werror -municode)
target_link_options(gesher-launch PRIVATE -municode)
add_library(gesher_shellext SHARED
  ${CMAKE_SOURCE_DIR}/compat/shell/gesher_shellext.cc
  ${CMAKE_SOURCE_DIR}/compat/shell/gesher_shellext.def)
set_target_properties(gesher_shellext PROPERTIES PREFIX "")
target_compile_options(gesher_shellext PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
target_link_libraries(gesher_shellext PRIVATE ole32)
