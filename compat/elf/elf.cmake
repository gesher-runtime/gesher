# gesher: the ELF loader for x86-64 Android .so files on Windows.
add_library(gesher_elf STATIC
  ${CMAKE_CURRENT_LIST_DIR}/elf_loader.cc
  ${CMAKE_CURRENT_LIST_DIR}/fs_rewrite.cc
  ${CMAKE_CURRENT_LIST_DIR}/tls.cc
  ${CMAKE_CURRENT_LIST_DIR}/x86_decode.cc)
target_include_directories(gesher_elf PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_include_directories(gesher_elf PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(gesher_elf PRIVATE -Wall -Wextra -Werror)
# gesher-exec.exe, the launcher for ELF executables (compat/elf/exec).
include(${CMAKE_CURRENT_LIST_DIR}/exec/exec.cmake)
