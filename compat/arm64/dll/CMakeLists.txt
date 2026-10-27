# gesher_arm64.dll: the arm64 translator (dynarmic). Built on its own, not part
# of the main Gesher build graph (dalvikvm LoadLibrary()s it), so dynarmic and
# its externals stay out of everything else. arm64.yml configures this with the
# llvm-mingw toolchain, pointing DYNARMIC_DIR at a checkout of the pinned
# dynarmic (docs/decisions/0047-arm64-translation.md).
#
#   cmake -S compat/arm64/dll -B out-arm64 -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-llvm-mingw.cmake -DLLVM_MINGW=... \
#     -DDYNARMIC_DIR=<dynarmic checkout> -DBoost_INCLUDE_DIR=<boost headers>
#
# docs/arm64.md has the build steps and the patches arm64.yml applies to
# dynarmic's vendored externals for the pinned llvm-mingw clang.
cmake_minimum_required(VERSION 3.24)
project(gesher_arm64_dll LANGUAGES C CXX ASM)

if(NOT WIN32)
  message(FATAL_ERROR "gesher_arm64.dll targets Windows")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT DYNARMIC_DIR)
  message(FATAL_ERROR "set -DDYNARMIC_DIR to a checkout of dynarmic (docs/arm64.md)")
endif()

# dynarmic: A64 frontend, x86-64 backend only; no tests. Its externals build
# with the pinned llvm-mingw clang after arm64.yml's patches (fmt
# FMT_USE_CONSTEVAL=0, mcl lift_sequence); those flags are set here so the
# add_subdirectory build sees them.
set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_compile_options(-DFMT_USE_CONSTEVAL=0 -Wno-deprecated-literal-operator
  -Wno-deprecated-declarations)

add_subdirectory(${DYNARMIC_DIR} dynarmic-build EXCLUDE_FROM_ALL)

# The DLL: one public export, gesher_arm64_init (and gesher_arm64_bind).
add_library(gesher_arm64 SHARED
  ${CMAKE_CURRENT_LIST_DIR}/translator.cc
  ${CMAKE_CURRENT_LIST_DIR}/guest_libc.cc
  ${CMAKE_CURRENT_LIST_DIR}/thunks.S)
set_target_properties(gesher_arm64 PROPERTIES PREFIX "" OUTPUT_NAME "gesher_arm64")
target_include_directories(gesher_arm64 PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../include ${DYNARMIC_DIR}/src)
target_link_libraries(gesher_arm64 PRIVATE dynarmic synchronization)
target_compile_options(gesher_arm64 PRIVATE -Wall -Wextra -Werror -mcx16)
# A self-contained DLL: the CRT and libunwind are linked statically, as Gesher's
# executables are, so it loads next to dalvikvm with no extra runtime files.
target_link_options(gesher_arm64 PRIVATE -static -Wl,--kill-at)

# A standalone smoke test (step 1): loads the DLL like the real loader and runs
# hand-assembled guest functions. Built here, run by tracks/47-arm64.ps1.
add_executable(arm64_smoke_test ${CMAKE_CURRENT_LIST_DIR}/arm64_smoke_test.cc)
target_include_directories(arm64_smoke_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../include)
target_compile_options(arm64_smoke_test PRIVATE -Wall -Wextra)
target_link_options(arm64_smoke_test PRIVATE -static)

add_executable(arm64_fpcr_test ${CMAKE_CURRENT_LIST_DIR}/arm64_fpcr_test.cc)
target_include_directories(arm64_fpcr_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../include)
target_compile_options(arm64_fpcr_test PRIVATE -Wall -Wextra -Werror)
target_link_options(arm64_fpcr_test PRIVATE -static)

add_executable(arm64_fpsr_test ${CMAKE_CURRENT_LIST_DIR}/arm64_fpsr_test.cc)
target_include_directories(arm64_fpsr_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../include)
target_compile_options(arm64_fpsr_test PRIVATE -Wall -Wextra -Werror)
target_link_options(arm64_fpsr_test PRIVATE -static)
