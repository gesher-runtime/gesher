# gesher: the ELF loader for x86-64 Android .so files on Windows.
add_library(gesher_elf STATIC
  ${CMAKE_CURRENT_LIST_DIR}/elf_loader.cc
  ${CMAKE_CURRENT_LIST_DIR}/tls.cc)
target_include_directories(gesher_elf PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_include_directories(gesher_elf PRIVATE ${CMAKE_CURRENT_LIST_DIR})
# WRFSBASE/RDFSBASE in the TLS setup need the assembler to accept them.
target_compile_options(gesher_elf PRIVATE -Wall -Wextra -Werror -mfsgsbase)
