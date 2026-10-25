# gesher: the ELF loader for x86-64 and arm64-v8a Android .so files on Windows.
# arm64_runtime.cc is the front end to gesher_arm64.dll (compat/arm64, Phase 7):
# the loader calls it for EM_AARCH64 objects. It is built in here so the loader
# and its arm64 glue are one library (ADR 0047, docs/arm64.md).
add_library(gesher_elf STATIC
  ${CMAKE_CURRENT_LIST_DIR}/elf_loader.cc
  ${CMAKE_CURRENT_LIST_DIR}/fs_rewrite.cc
  ${CMAKE_CURRENT_LIST_DIR}/tls.cc
  ${CMAKE_CURRENT_LIST_DIR}/x86_decode.cc
  ${CMAKE_CURRENT_LIST_DIR}/../arm64/arm64_runtime.cc)
target_include_directories(gesher_elf PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include
  ${CMAKE_CURRENT_LIST_DIR}/../arm64/include)
target_include_directories(gesher_elf PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(gesher_elf PRIVATE -Wall -Wextra -Werror)
# gesher-exec.exe, the launcher for ELF executables (compat/elf/exec).
include(${CMAKE_CURRENT_LIST_DIR}/exec/exec.cmake)
