# Gesher: Windows shell integration of installed Android apps (shell track,
# docs/installer.md): com.gesher.app.shell.Shell's natives, in
# libandroid_runtime's builtin library (android_runtime.cc registers them).
# shell_win32.cc uses COM (ole32, uuid) and the registry (advapi32: the apps'
# URL protocols) and finds shell32's functions at run time.
target_sources(framework_runtime PRIVATE
  ${CMAKE_SOURCE_DIR}/compat/shell/shell_natives.cc
  ${CMAKE_SOURCE_DIR}/compat/shell/shell_win32.cc)
target_include_directories(framework_runtime PRIVATE ${CMAKE_SOURCE_DIR}/compat/shell)
target_link_libraries(framework_runtime PUBLIC ole32 uuid advapi32)

# gesher-launch.exe: what an app's shortcut, a double-clicked .apk and a toast
# click start (Windows subsystem: no console window flashes; it hands on the
# foreground to the app's window; a launch shows a splash window until the
# app's window is up: GDI, shell32's CommandLineToArgvW, COM for the windows'
# property stores; a double-clicked .apk is installed only after its
# confirmation dialog, compat/shell/apk_info_file.h reading `gesher-app
# apk-info`). gesher_shellext.dll: the .apk file type's icon handler,
# loaded by Explorer (linked statically, as everything here).
configure_file(${CMAKE_SOURCE_DIR}/compat/shell/gesher_launch.rc.in
  ${CMAKE_CURRENT_BINARY_DIR}/gesher_launch.rc @ONLY)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gesher_launch_icon.obj
  COMMAND ${CMAKE_RC_COMPILER} -i ${CMAKE_CURRENT_BINARY_DIR}/gesher_launch.rc
    -o ${CMAKE_CURRENT_BINARY_DIR}/gesher_launch_icon.obj -O coff
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/gesher_launch.rc
    ${CMAKE_SOURCE_DIR}/tools/package/gesher.ico
  VERBATIM)
add_executable(gesher-launch WIN32 ${CMAKE_SOURCE_DIR}/compat/shell/gesher_launch.cc
  ${CMAKE_CURRENT_BINARY_DIR}/gesher_launch_icon.obj)
target_compile_options(gesher-launch PRIVATE -Wall -Wextra -Werror -municode)
target_link_options(gesher-launch PRIVATE -municode)
target_link_libraries(gesher-launch PRIVATE user32 gdi32 shell32 ole32 advapi32)
add_executable(gesher-app ${CMAKE_SOURCE_DIR}/compat/shell/gesher_app.cc)
target_compile_options(gesher-app PRIVATE -Wall -Wextra -Werror -municode)
target_link_options(gesher-app PRIVATE -municode)
target_link_libraries(gesher-app PRIVATE advapi32)
add_library(gesher_shellext SHARED
  ${CMAKE_SOURCE_DIR}/compat/shell/gesher_shellext.cc
  ${CMAKE_SOURCE_DIR}/compat/shell/gesher_shellext.def)
set_target_properties(gesher_shellext PROPERTIES PREFIX "")
target_compile_options(gesher_shellext PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
target_link_libraries(gesher_shellext PRIVATE ole32)
