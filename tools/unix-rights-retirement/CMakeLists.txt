cmake_minimum_required(VERSION 3.24)
project(gesher_unix_rights_retirement LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(WIN32)
  get_filename_component(GESHER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
  set(AOSP_DIR "${GESHER_ROOT}/third_party/aosp" CACHE PATH "Pinned component AOSP inputs")
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  add_compile_definitions(
    GESHER USE_MINGW WIN32_LEAN_AND_MEAN NOMINMAX NOGDI _USE_MATH_DEFINES
    __STDC_FORMAT_MACROS __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 _FILE_OFFSET_BITS=64
    _POSIX_THREAD_SAFE_FUNCTIONS)
  add_compile_options(-mno-ms-bitfields -fno-exceptions
    $<$<COMPILE_LANGUAGE:CXX>:-fsized-deallocation>)
  add_link_options(-static)

  include("${GESHER_ROOT}/compat/posix/posix.cmake")
  add_executable(binderd "${GESHER_ROOT}/compat/binder/binderd.cc"
    "${GESHER_ROOT}/compat/framework/app_container.cc")
  target_compile_options(binderd PRIVATE -Wall -Wextra -Werror
    "SHELL:-idirafter ${AOSP_DIR}/bionic/libc/kernel/uapi")
  target_link_libraries(binderd PRIVATE bcrypt advapi32 ole32)
endif()

foreach(probe last_close transferred_close socket_identity)
  add_executable(${probe} "${probe}.c")
  target_compile_options(${probe} PRIVATE -Wall -Wextra -Werror)
  if(WIN32)
    target_link_libraries(${probe} PRIVATE gesher_posix)
  endif()
endforeach()
