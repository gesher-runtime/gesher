# gesher-exec: Android ELF executables as Windows processes (docs/exec.md).
# gesher_exec_spawn finds and starts programs as execvp would; libcore's
# process natives (compat/libcore/process.cc) link it into dalvikvm, and the
# launcher gesher-exec.exe uses it for its minimal shell. The launcher loads
# an ELF executable with the ELF loader and the bionic shim as its main
# program.
add_library(gesher_exec_spawn STATIC ${CMAKE_CURRENT_LIST_DIR}/exec_spawn.cc)
target_include_directories(gesher_exec_spawn PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(gesher_exec_spawn PRIVATE -Wall -Wextra -Werror)

add_executable(gesher-exec ${CMAKE_CURRENT_LIST_DIR}/gesher_exec.cc)
target_compile_options(gesher-exec PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gesher-exec PRIVATE gesher_exec_spawn gesher_bionic gesher_elf bcrypt shell32)
