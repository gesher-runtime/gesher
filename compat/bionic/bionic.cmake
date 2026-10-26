# gesher: the bionic shim, the libc/libm/libdl/liblog/pthread symbols an
# Android x86-64 .so imports, for the ELF loader (compat/elf). Phase 6.
add_library(gesher_bionic_unicode STATIC
  ${CMAKE_CURRENT_LIST_DIR}/unicode_bionic.cc
  ${CMAKE_CURRENT_LIST_DIR}/unicode_width.cc)
target_link_libraries(gesher_bionic_unicode PRIVATE icuuc base log gesher_posix)
target_compile_options(gesher_bionic_unicode PRIVATE -Wall -Wextra -Werror)

add_library(gesher_bionic STATIC
  ${CMAKE_CURRENT_LIST_DIR}/extra_bionic.cc
  ${CMAKE_CURRENT_LIST_DIR}/extra_sys_bionic.cc
  ${CMAKE_CURRENT_LIST_DIR}/locale_bionic.cc
  ${CMAKE_CURRENT_LIST_DIR}/posix_bionic.cc
  ${CMAKE_CURRENT_LIST_DIR}/process_bionic.cc
  ${CMAKE_CURRENT_LIST_DIR}/pthread_bionic.cc
  ${CMAKE_CURRENT_LIST_DIR}/shim.cc
  ${CMAKE_CURRENT_LIST_DIR}/signal_bionic.cc
  ${CMAKE_CURRENT_LIST_DIR}/socket_bionic.cc
  ${CMAKE_CURRENT_LIST_DIR}/stdio_bionic.cc
  ${CMAKE_CURRENT_LIST_DIR}/time_bionic.cc
  ${CMAKE_CURRENT_LIST_DIR}/zlib_bionic.cc)
target_include_directories(gesher_bionic PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_include_directories(gesher_bionic PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(gesher_bionic PUBLIC gesher_elf gesher_exec_spawn synchronization bcrypt ws2_32)
# libz.so for app libraries: zlib_bionic.cc over external/zlib (third_party.cmake).
target_link_libraries(gesher_bionic PRIVATE z gesher_bionic_unicode)
target_compile_options(gesher_bionic PRIVATE -Wall -Wextra -Werror)
