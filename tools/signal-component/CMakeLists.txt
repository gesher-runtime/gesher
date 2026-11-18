cmake_minimum_required(VERSION 3.24)
project(gesher_signal_component LANGUAGES C CXX ASM)

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
add_executable(signal_job_runner job_runner.cc)
target_compile_options(signal_job_runner PRIVATE -Wall -Wextra -Werror)
target_link_options(signal_job_runner PRIVATE -municode)

include("${GESHER_ROOT}/compat/posix/posix.cmake")
add_executable(realtime_signals_test "${GESHER_ROOT}/tests/native/realtime_signals_test.cc")
add_executable(realtime_signal_contract_test "${GESHER_ROOT}/tests/exec/realtime_signals_contract.c")
foreach(target realtime_signals_test realtime_signal_contract_test)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(${target} PRIVATE gesher_posix)
endforeach()
