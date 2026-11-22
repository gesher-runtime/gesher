cmake_minimum_required(VERSION 3.24)
project(gesher_revocable_component LANGUAGES C CXX ASM)

# A component build of the same production POSIX sources and flags as the
# main project. No replacement descriptor, Binder, or Win32 implementations.
get_filename_component(GESHER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(AOSP_DIR "${GESHER_ROOT}/third_party/aosp" CACHE PATH "Pinned component AOSP inputs")
if(NOT WIN32)
  message(FATAL_ERROR "Use cmake/toolchain-llvm-mingw.cmake")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_definitions(
  GESHER USE_MINGW WIN32_LEAN_AND_MEAN NOMINMAX NOGDI _USE_MATH_DEFINES
  __STDC_FORMAT_MACROS __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 _FILE_OFFSET_BITS=64
  _POSIX_THREAD_SAFE_FUNCTIONS)
add_compile_options(-mno-ms-bitfields -fno-exceptions
  $<$<COMPILE_LANGUAGE:CXX>:-fsized-deallocation>)
add_link_options(-static)

# The supervisor is deliberately Win32-only: it owns the test process tree,
# while every contract fixture below links the actual Gesher implementation.
add_executable(revocable_job_runner job_runner.cc)
target_compile_options(revocable_job_runner PRIVATE -Wall -Wextra -Werror)
target_link_options(revocable_job_runner PRIVATE -municode)

include("${GESHER_ROOT}/compat/posix/posix.cmake")
add_executable(revocable_file_test "${GESHER_ROOT}/tests/native/revocable_file_test.cc")
add_executable(binderd "${GESHER_ROOT}/compat/binder/binderd.cc")
add_executable(binder_probe "${GESHER_ROOT}/probes/binder/binder_probe.cc")
add_executable(binder_sg_actor "${GESHER_ROOT}/tests/binder/sg-actor.cc")
add_executable(binder_sg_wire_peer "${GESHER_ROOT}/tests/binder/sg-wire-peer.cc")
foreach(target revocable_file_test binderd binder_probe binder_sg_actor binder_sg_wire_peer)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Werror
    "SHELL:-idirafter ${AOSP_DIR}/bionic/libc/kernel/uapi")
endforeach()
