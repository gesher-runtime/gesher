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

# gesher: the ELF loader for x86-64 Android .so files (Phase 6).
include(compat/elf/elf.cmake)
# gesher: the bionic shim the loaded .so files link against (Phase 6).
include(compat/bionic/bionic.cmake)

include(cmake/aosp/third_party.cmake)
include(cmake/aosp/system.cmake)
include(cmake/aosp/art.cmake)
include(cmake/aosp/art_runtime.cmake)
# dex2oat, for Gesher's boot image (docs/boot-image.md).
include(cmake/aosp/dex2oat.cmake)
include(cmake/aosp/icu.cmake)
# BoringSSL, for libcore's NativeBN and Conscrypt (docs/network.md).
include(cmake/aosp/boringssl.cmake)
include(cmake/aosp/libcore_native.cmake)
# Conscrypt's libjavacrypto and compat/net: TLS, Windows' trusted roots (docs/network.md).
include(cmake/aosp/conscrypt.cmake)
include(cmake/aosp/binder.cmake)
include(cmake/aosp/servicemanager.cmake)
include(cmake/aosp/framework.cmake)
# Framework natives real apps need: SQLite, android.database, animators (docs/e2e.md).
include(cmake/aosp/framework_e2e.cmake)
# gesher: Start menu shortcuts and AppUserModelIDs for installed apps (shell track).
include(cmake/aosp/shell.cmake)
# gesher: the device-state services' Windows side: alarms, clipboard, toasts, power (devsvc track).
include(cmake/aosp/devsvc.cmake)
# gesher: System.loadLibrary of an app's ELF .so in dalvikvm (Phase 6).
include(compat/bionic/jni.cmake)
include(cmake/aosp/googletest.cmake)
include(cmake/aosp/core_libs_tests.cmake)
# ART's own gtests for libartbase and libdexfile (docs/posix.md).
include(cmake/aosp/art_gtests.cmake)
include(cmake/aosp/graphics.cmake)
# Input (docs/input.md): libinput, the input natives, Win32 messages to Android events.
include(cmake/aosp/input.cmake)
# Media (docs/media.md): the audio engine and the framework's media natives.
include(cmake/aosp/media.cmake)
# Devices (docs/devices.md): location, sensors and cameras over Windows' APIs.
include(compat/devices/devices.cmake)
# android.webkit.WebView on WebView2 (docs/webview.md): libgesher_webview.so, builtin.
include(cmake/webview.cmake)

# Phase 0 probe: Windows primitives ART relies on.
add_executable(platform_probe probes/platform/platform_probe.c)
target_compile_options(platform_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(platform_probe PRIVATE synchronization)

# gesher-base's socket layer and fd overrides, run in CI on Windows.
add_executable(socket_probe probes/posix/socket_probe.cc)
target_compile_options(socket_probe PRIVATE -Wall -Wextra -Werror)

# gesher-base's mmap layer: placeholder mappings, executable file views
# (stack track, docs/decisions/0036); run in CI on Windows.
add_executable(mman_probe probes/posix/mman_probe.cc)
target_compile_options(mman_probe PRIVATE -Wall -Wextra -Werror)

# eventfd and epoll, for libutils' Looper; also run in CI on Windows.
add_executable(epoll_probe probes/posix/epoll_probe.cc)
target_compile_options(epoll_probe PRIVATE -Wall -Wextra -Werror)

# gesher-base's paths, /proc, /etc/timezone, inotify and process model
# (docs/posix.md); also run in CI on Windows.
add_executable(posix_probe probes/posix/posix_probe.cc)
target_compile_options(posix_probe PRIVATE -Wall -Wextra -Werror)

# libutils' Looper over gesher-base's epoll and eventfd, RefBase, String8;
# also run in CI on Windows.
add_executable(libutils_probe probes/system/libutils_probe.cc)
target_compile_options(libutils_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(libutils_probe PRIVATE utils)

# Phase 6: can Windows user code use %fs for bionic's TLS (WRFSBASE)?
add_executable(fsbase_probe probes/elf/fsbase_probe.cc)
target_compile_options(fsbase_probe PRIVATE -Wall -Wextra -Werror -mfsgsbase)

# Phase 6: the ELF loader loads an x86-64 Android .so and calls its exports.
add_executable(elf_load_test tests/native/elf_load_test.cc)
target_compile_options(elf_load_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(elf_load_test PRIVATE gesher_bionic gesher_elf)

# Live CRT environment storage after host and guest mutations (exec4).
add_executable(exec4_environment_test tests/exec/environment-host-test.cc)
target_compile_options(exec4_environment_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(exec4_environment_test PRIVATE gesher_bionic gesher_elf)

# All-code-point comparison with the independent pinned ICU4X reference.
add_executable(native3_unicode_test tests/native/native3_unicode_test.cc)
target_compile_options(native3_unicode_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(native3_unicode_test PRIVATE gesher_bionic gesher_elf icuuc)

add_executable(native3_cpu_clock_test tests/native/native3_cpu_clock_test.cc)
target_compile_options(native3_cpu_clock_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(native3_cpu_clock_test PRIVATE artbase gesher_posix)

# The same source calls actual Linux fcntl in the independent reference job.
# This Windows target links the actual common POSIX fd/stdio/lock code.
add_executable(native3_lock_command_worker tests/native3-locks/command_worker.cc)
target_compile_options(native3_lock_command_worker PRIVATE -Wall -Wextra -Werror)
target_link_libraries(native3_lock_command_worker PRIVATE gesher_posix)

add_executable(native3_lock_duplicate_test tests/native3-locks/duplicate_windows_test.cc)
target_compile_options(native3_lock_duplicate_test PRIVATE -Wall -Wextra -Werror
  "SHELL:-idirafter ${AOSP_DIR}/bionic/libc/kernel/uapi")
target_link_libraries(native3_lock_duplicate_test PRIVATE gesher_posix)

# Isolated temporal diagnostic, never part of the accepted13-mode supplier.
add_executable(native3_cloexec_window_probe EXCLUDE_FROM_ALL tests/native3-locks/cloexec_window_probe.cc)
target_compile_options(native3_cloexec_window_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(native3_cloexec_window_probe PRIVATE gesher_posix)

# Independent kernel-HANDLE oracle. The directory globally adds gesher_posix
# and its shadow headers; explicitly remove both for this CRT-free child.
add_executable(native3_cloexec_raw_child EXCLUDE_FROM_ALL tests/native3-locks/cloexec_raw_child.cc)
set_property(TARGET native3_cloexec_raw_child PROPERTY LINK_LIBRARIES "")
set_property(TARGET native3_cloexec_raw_child PROPERTY INCLUDE_DIRECTORIES "")
target_compile_options(native3_cloexec_raw_child PRIVATE -Wall -Wextra -Werror -fno-builtin -fno-stack-protector)
target_link_options(native3_cloexec_raw_child PRIVATE -nostdlib -Wl,--entry,cloexec_child_entry -Wl,--subsystem,windows)
target_link_libraries(native3_cloexec_raw_child PRIVATE kernel32)
add_dependencies(native3_cloexec_window_probe native3_cloexec_raw_child)

# Phase 7: the ELF loader loads an arm64-v8a Android .so and the arm64
# translator (gesher_arm64.dll) runs it (docs/arm64.md, ADR 0047).
add_executable(arm64_elf_test tests/native/arm64_elf_test.cc)
target_compile_options(arm64_elf_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(arm64_elf_test PRIVATE gesher_bionic gesher_elf)
