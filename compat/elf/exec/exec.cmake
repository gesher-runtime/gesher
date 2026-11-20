# gesher-exec: Android ELF executables as Windows processes (docs/exec.md).
# gesher_exec_spawn finds and starts programs as execvp would; libcore's
# process natives (compat/libcore/process.cc) link it into dalvikvm, and the
# launcher gesher-exec.exe uses it for its shell. The launcher loads
# an ELF executable with the ELF loader and the bionic shim as its main
# program.
add_library(gesher_exec_spawn STATIC
  ${CMAKE_CURRENT_LIST_DIR}/exec_spawn.cc
  ${CMAKE_CURRENT_LIST_DIR}/gesher_tools.cc)
target_include_directories(gesher_exec_spawn PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(gesher_exec_spawn PUBLIC gesher_posix)
target_compile_options(gesher_exec_spawn PRIVATE -Wall -Wextra -Werror)
# Launch allocations unwind owned handles and native attribute lists, then
# return ERROR_NOT_ENOUGH_MEMORY at the Spawn/SpawnWithFds status boundaries.
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/exec_spawn.cc PROPERTIES
  COMPILE_OPTIONS "-fexceptions")

add_executable(ofd_native_child ${CMAKE_SOURCE_DIR}/tests/exec/ofd-native-child.cc)
# posix.cmake supplies a directory-wide link_libraries default. This fixture
# must remain an ordinary Windows executable without the Gesher importer.
set_property(TARGET ofd_native_child PROPERTY LINK_LIBRARIES "")
target_compile_options(ofd_native_child PRIVATE -Wall -Wextra -Werror)
add_executable(ofd_spawn_test ${CMAKE_SOURCE_DIR}/tests/exec/ofd-spawn-test.cc)
target_compile_options(ofd_spawn_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ofd_spawn_test PRIVATE gesher_exec_spawn)
add_dependencies(ofd_spawn_test ofd_native_child)

add_executable(pipe_native_process_test ${CMAKE_SOURCE_DIR}/tests/native/pipe_native_process_test.cc)
target_compile_options(pipe_native_process_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(pipe_native_process_test PRIVATE gesher_exec_spawn)
add_dependencies(pipe_native_process_test ofd_native_child)

add_executable(gesher-exec
  ${CMAKE_CURRENT_LIST_DIR}/gesher_exec.cc
  ${CMAKE_CURRENT_LIST_DIR}/spawn_actions.cc
  ${CMAKE_CURRENT_LIST_DIR}/gesher_shell.cc)
target_compile_options(gesher-exec PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gesher-exec PRIVATE gesher_exec_spawn gesher_bionic gesher_elf bcrypt shell32)
add_dependencies(ofd_spawn_test gesher-exec)

add_executable(exec4_no_console_probe ${CMAKE_SOURCE_DIR}/tests/exec/no-console-spawn.cc)
target_compile_options(exec4_no_console_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(exec4_no_console_probe PRIVATE gesher_exec_spawn gesher_bionic gesher_elf shell32)

add_executable(exec3_tls_probe ${CMAKE_SOURCE_DIR}/tests/exec/fs-local-exec-probe.cc)
target_include_directories(exec3_tls_probe PRIVATE ${CMAKE_CURRENT_LIST_DIR}/..)
target_compile_options(exec3_tls_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(exec3_tls_probe PRIVATE gesher_elf)

add_executable(exec3_binder_ready_probe
  ${CMAKE_SOURCE_DIR}/tests/exec/binder-ready-probe.cc
  ${CMAKE_SOURCE_DIR}/compat/framework/services_zygote.cc)
target_include_directories(exec3_binder_ready_probe PRIVATE
  ${CMAKE_SOURCE_DIR}/compat/framework ${CMAKE_SOURCE_DIR}/compat)
target_compile_options(exec3_binder_ready_probe PRIVATE -Wall -Wextra -Werror)

add_executable(exec3_property_host_probe ${CMAKE_SOURCE_DIR}/tests/exec/property-host-probe.cc)
target_compile_options(exec3_property_host_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(exec3_property_host_probe PRIVATE utils)
