cmake_minimum_required(VERSION 3.24)
project(xattr_fd_component LANGUAGES C CXX)
if(NOT WIN32 OR NOT IS_DIRECTORY "${AOSP_DIR}")
  message(FATAL_ERROR "A Windows cross compiler and verified AOSP input directory are required")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Match the production POSIX compilation profile. Include the real target so a
# source addition or linker requirement cannot silently leave this gate stale.
add_compile_definitions(GESHER USE_MINGW WIN32_LEAN_AND_MEAN NOMINMAX NOGDI _USE_MATH_DEFINES
  __STDC_FORMAT_MACROS __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 _FILE_OFFSET_BITS=64 _POSIX_THREAD_SAFE_FUNCTIONS)
add_compile_options(-mno-ms-bitfields -fno-exceptions $<$<COMPILE_LANGUAGE:CXX>:-fsized-deallocation>)
add_link_options(-static)
get_filename_component(GESHER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${GESHER_ROOT}/compat/posix/posix.cmake")
add_executable(xattr_fd_host_test "${GESHER_ROOT}/tests/native/xattr_fd_host_test.cc")
target_compile_options(xattr_fd_host_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(xattr_fd_host_test PRIVATE gesher_posix advapi32)
