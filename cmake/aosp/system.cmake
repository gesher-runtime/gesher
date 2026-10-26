# system/* libraries, mirroring their Android.bp Windows configuration.

set(_core "${AOSP_DIR}/system/core")

# --- libbase ----------------------------------------------------------------
set(_b "${AOSP_DIR}/system/libbase")
add_library(base STATIC
  ${_b}/chrono_utils.cpp ${_b}/file.cpp ${_b}/hex.cpp ${_b}/logging.cpp
  ${_b}/mapped_file.cpp ${_b}/parsebool.cpp ${_b}/parsenetaddress.cpp
  ${_b}/posix_strerror_r.cpp ${_b}/process.cpp ${_b}/properties.cpp
  ${_b}/result.cpp ${_b}/stringprintf.cpp ${_b}/strings.cpp ${_b}/threads.cpp
  ${_b}/test_utils.cpp
  # windows-only
  ${_b}/errors_windows.cpp ${_b}/utf8.cpp
  ${CMAKE_SOURCE_DIR}/compat/posix/src/base_properties.cc)
target_include_directories(base PUBLIC ${_b}/include)
target_link_libraries(base PUBLIC fmtlib log pthread)
target_compile_options(base PRIVATE -Wall -Wextra)

# --- liblog -----------------------------------------------------------------
set(_l "${AOSP_DIR}/system/logging/liblog")
add_library(log STATIC
  ${_l}/log_event_list.cpp ${_l}/log_event_write.cpp ${_l}/logger_name.cpp
  ${_l}/logger_read.cpp ${CMAKE_SOURCE_DIR}/compat/posix/src/android_log_write.cc ${_l}/logprint.cpp
  ${CMAKE_SOURCE_DIR}/compat/posix/src/android_log.cc)
target_include_directories(log PUBLIC ${_l}/include)
target_include_directories(log PRIVATE ${_l} ${_b}/include ${_core}/libcutils/include)
target_link_options(log INTERFACE -Wl,-u,gesher_android_log_anchor)
target_compile_definitions(log PRIVATE
  LIBLOG_LOG_TAG=1006 SNET_EVENT_LOG_TAG=1397638484 ANDROID_DEBUGGABLE=0)
target_compile_options(log PRIVATE -Wall -Wextra)

# --- libziparchive ----------------------------------------------------------
set(_zip "${AOSP_DIR}/system/libziparchive")
add_library(ziparchive STATIC
  ${_zip}/zip_archive.cc ${_zip}/zip_archive_stream_entry.cc
  ${_zip}/zip_cd_entry_map.cc ${_zip}/zip_error.cpp ${_zip}/zip_writer.cc)
target_include_directories(ziparchive PUBLIC ${_zip}/include ${CMAKE_SOURCE_DIR}/compat/include)
target_include_directories(ziparchive PRIVATE ${_zip}/incfs_support/include)
target_link_libraries(ziparchive PUBLIC base log z)
target_compile_definitions(ziparchive PRIVATE ZLIB_CONST)
target_compile_options(ziparchive PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers -Wconversion -Wno-sign-conversion>)

# --- libcutils (system/core/libcutils/Android.bp) ----------------------------
# The device's source set (docs/core-libs.md): the common sources, the linux,
# not_windows and android ones, and libcutils_sockets' android set, as
# gesher-base's sockets are file descriptors with Linux's ABI (system/core
# patch 0002). The device's ashmem-dev.cpp and trace-dev.cpp drive kernel
# interfaces (the ashmem driver or sealed memfds, tracefs and bionic's
# property area); the host's ashmem-host.cpp and trace-host.cpp stand in.
set(_cu "${_core}/libcutils")
add_library(cutils STATIC
  ${_cu}/config_utils.cpp ${_cu}/iosched_policy.cpp ${_cu}/load_file.cpp
  ${_cu}/native_handle.cpp ${_cu}/properties.cpp ${_cu}/record_stream.cpp
  ${_cu}/strlcpy.c
  # linux
  ${_cu}/canned_fs_config.cpp ${_cu}/fs_config.cpp
  # not_windows (libcutils_nonwindows_sources)
  ${_cu}/fs.cpp ${_cu}/hashmap.cpp ${_cu}/multiuser.cpp ${_cu}/str_parms.cpp
  # android, less the kernel-specific ashmem-dev.cpp and trace-dev.cpp
  ${_cu}/android_reboot.cpp ${_cu}/klog.cpp ${_cu}/partition_utils.cpp
  ${_cu}/qtaguid.cpp ${_cu}/uevent.cpp
  # host
  ${_cu}/trace-host.cpp ${_cu}/ashmem-host.cpp
  # libcutils_sockets (android)
  ${_cu}/sockets.cpp ${_cu}/android_get_control_file.cpp
  ${_cu}/socket_inaddr_any_server_unix.cpp ${_cu}/socket_local_client_unix.cpp
  ${_cu}/socket_local_server_unix.cpp ${_cu}/socket_network_client_unix.cpp
  ${_cu}/sockets_unix.cpp)
target_include_directories(cutils PUBLIC ${_cu}/include ${_core}/libprocessgroup/include)
# ashmem-host.cpp includes utils/Compat.h (headers only; no link dependency).
target_include_directories(cutils PRIVATE ${_core}/libutils/include)
target_link_libraries(cutils PUBLIC base log)
target_compile_definitions(cutils PRIVATE _GNU_SOURCE)
target_compile_options(cutils PRIVATE -Wall -Wextra -Wno-exit-time-destructors)

# --- libutils_binder, libutils (system/core/libutils/Android.bp) -------------
# The device's source sets: Trace.cpp (android) and Looper.cpp (linux) too;
# gesher-base's epoll and eventfd (docs/decisions/0009-socket-fds.md) let
# Looper build for Windows.
set(_ut "${_core}/libutils")
set(_utils_flags -Wall -Wno-exit-time-destructors -Wno-unused-private-field)
set(_utils_defs ANDROID_UTILS_REF_BASE_DISABLE_IMPLICIT_CONSTRUCTION MB_CUR_MAX=1)
set(_utils_binder_srcs
  ${_ut}/binder/Errors.cpp ${_ut}/binder/RefBase.cpp ${_ut}/binder/SharedBuffer.cpp
  ${_ut}/binder/String16.cpp ${_ut}/binder/String8.cpp ${_ut}/binder/StrongPointer.cpp
  ${_ut}/binder/Unicode.cpp ${_ut}/binder/VectorImpl.cpp)
set(_utils_binder_flags ${_utils_flags} -Winvalid-offsetof -Wsequence-point
  -Wzero-as-null-pointer-constant)
add_library(utils_binder STATIC ${_utils_binder_srcs})
target_include_directories(utils_binder PUBLIC ${_ut}/binder/include)
target_link_libraries(utils_binder PUBLIC cutils log base)
target_compile_definitions(utils_binder PRIVATE ${_utils_defs})
target_compile_options(utils_binder PRIVATE ${_utils_binder_flags})

# libutils_binder_custom_allocator, for its test (cmake/aosp/core_libs_tests.cmake).
add_library(utils_binder_custom_allocator STATIC EXCLUDE_FROM_ALL ${_utils_binder_srcs})
target_include_directories(utils_binder_custom_allocator PUBLIC ${_ut}/binder/include)
target_link_libraries(utils_binder_custom_allocator PUBLIC cutils log base)
target_compile_definitions(utils_binder_custom_allocator PRIVATE ${_utils_defs}
  ANDROID_UTILS_CUSTOM_ALLOCATOR)
target_compile_options(utils_binder_custom_allocator PRIVATE ${_utils_binder_flags})

add_library(utils STATIC
  ${_ut}/FileMap.cpp ${_ut}/JenkinsHash.cpp ${_ut}/LightRefBase.cpp
  ${_ut}/NativeHandle.cpp ${_ut}/Printer.cpp ${_ut}/StopWatch.cpp
  ${_ut}/SystemClock.cpp ${_ut}/Threads.cpp ${_ut}/Timers.cpp ${_ut}/Tokenizer.cpp
  ${CMAKE_SOURCE_DIR}/compat/posix/src/sysprop_callbacks.cc
  # android
  ${_ut}/Trace.cpp
  # linux
  ${_ut}/Looper.cpp)
target_include_directories(utils PUBLIC ${_ut}/include ${_core}/libsystem/include)
target_link_libraries(utils PUBLIC utils_binder cutils log base)
target_compile_definitions(utils PRIVATE ${_utils_defs})
target_compile_options(utils PRIVATE ${_utils_flags})

# --- libutilscallstack -----------------------------------------------------------
# Disabled for Windows in Android.bp, as libunwindstack is ELF's. Gesher's
# AndroidUnwinder (compat/unwindstack) walks Windows' x64 unwind data instead;
# ProcessCallStack.cpp (linux) builds too.
add_library(unwindstack_gesher STATIC ${CMAKE_SOURCE_DIR}/compat/unwindstack/android_unwinder.cc)
target_include_directories(unwindstack_gesher PUBLIC
  ${AOSP_DIR}/system/unwinding/libunwindstack/include)
target_link_libraries(unwindstack_gesher PUBLIC base)
target_compile_options(unwindstack_gesher PRIVATE -Wall -Wextra -Werror)

add_library(utilscallstack STATIC ${_ut}/CallStack.cpp ${_ut}/ProcessCallStack.cpp)
target_link_libraries(utilscallstack PUBLIC utils unwindstack_gesher log)
target_compile_definitions(utilscallstack PRIVATE ${_utils_defs})
target_compile_options(utilscallstack PRIVATE ${_utils_flags})
