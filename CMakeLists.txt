cmake_minimum_required(VERSION 3.24)
project(gesher LANGUAGES C CXX ASM)

# AOSP sources come from tools/fetch-aosp.sh, never from this repository.
set(AOSP_DIR "${CMAKE_SOURCE_DIR}/third_party/aosp" CACHE PATH "AOSP checkout (tools/fetch-aosp.sh)")
if(NOT EXISTS "${AOSP_DIR}/art/libartbase")
  message(FATAL_ERROR "AOSP sources not found in ${AOSP_DIR}; run tools/fetch-aosp.sh")
endif()

if(NOT WIN32)
  message(FATAL_ERROR "Gesher targets Windows; configure with -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-llvm-mingw.cmake")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)    # AOSP builds with -std=gnu++20
set(CMAKE_C_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Flags Soong applies to every Windows host module.
add_compile_definitions(
  GESHER USE_MINGW WIN32_LEAN_AND_MEAN NOMINMAX NOGDI _USE_MATH_DEFINES
  __STDC_FORMAT_MACROS __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 _FILE_OFFSET_BITS=64
  _POSIX_THREAD_SAFE_FUNCTIONS)
add_compile_options(-mno-ms-bitfields -fno-exceptions
  $<$<COMPILE_LANGUAGE:CXX>:-fsized-deallocation>)
# Everything links statically: one self-contained .exe per tool. Executables
# are stripped by default; objects keep their debug info. Local builds that
# need symbols (addr2line on crash reports) turn GESHER_STRIP off.
option(GESHER_STRIP "Strip executables" ON)
add_link_options(-static)
if(GESHER_STRIP)
  add_link_options(-s)
endif()

# gesher-base: POSIX/Linux interfaces implemented on Win32.
include(compat/posix/posix.cmake)

include(cmake/aosp/third_party.cmake)
include(cmake/aosp/system.cmake)
include(cmake/aosp/art.cmake)
include(cmake/aosp/art_runtime.cmake)

# Phase 0 probe: Windows primitives ART relies on.
add_executable(platform_probe probes/platform/platform_probe.c)
target_compile_options(platform_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(platform_probe PRIVATE synchronization)
