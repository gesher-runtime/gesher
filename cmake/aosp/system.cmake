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
  ${_b}/errors_windows.cpp ${_b}/utf8.cpp)
target_include_directories(base PUBLIC ${_b}/include)
target_link_libraries(base PUBLIC fmtlib log pthread)
target_compile_options(base PRIVATE -Wall -Wextra)

# --- liblog -----------------------------------------------------------------
set(_l "${AOSP_DIR}/system/logging/liblog")
add_library(log STATIC
  ${_l}/log_event_list.cpp ${_l}/log_event_write.cpp ${_l}/logger_name.cpp
  ${_l}/logger_read.cpp ${_l}/logger_write.cpp ${_l}/logprint.cpp
  ${_l}/properties.cpp)
target_include_directories(log PUBLIC ${_l}/include)
target_include_directories(log PRIVATE ${_b}/include ${_core}/libcutils/include)
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

# --- libcutils (Windows configuration, plus the host trace and ashmem) ------
set(_cu "${_core}/libcutils")
add_library(cutils STATIC
  ${_cu}/config_utils.cpp ${_cu}/iosched_policy.cpp ${_cu}/load_file.cpp
  ${_cu}/native_handle.cpp ${_cu}/properties.cpp ${_cu}/record_stream.cpp
  ${_cu}/strlcpy.c
  # host
  ${_cu}/trace-host.cpp ${_cu}/ashmem-host.cpp
  # libcutils_sockets, windows
  ${_cu}/sockets.cpp ${_cu}/socket_inaddr_any_server_windows.cpp
  ${_cu}/socket_network_client_windows.cpp ${_cu}/sockets_windows.cpp)
target_include_directories(cutils PUBLIC ${_cu}/include ${_core}/libprocessgroup/include)
target_link_libraries(cutils PUBLIC base log)
target_compile_definitions(cutils PRIVATE _GNU_SOURCE)
target_compile_options(cutils PRIVATE -Wall -Wextra -Wno-exit-time-destructors)

# --- libutils_binder, libutils (Windows configuration, plus Looper) ---------
# Looper.cpp is Linux-only in Android.bp; gesher-base's epoll and eventfd
# (docs/decisions/0009-socket-fds.md) let it build for Windows.
set(_ut "${_core}/libutils")
set(_utils_flags -Wall -Wno-exit-time-destructors -Wno-unused-private-field)
set(_utils_defs ANDROID_UTILS_REF_BASE_DISABLE_IMPLICIT_CONSTRUCTION MB_CUR_MAX=1)
add_library(utils_binder STATIC
  ${_ut}/binder/Errors.cpp ${_ut}/binder/RefBase.cpp ${_ut}/binder/SharedBuffer.cpp
  ${_ut}/binder/String16.cpp ${_ut}/binder/String8.cpp ${_ut}/binder/StrongPointer.cpp
  ${_ut}/binder/Unicode.cpp ${_ut}/binder/VectorImpl.cpp)
target_include_directories(utils_binder PUBLIC ${_ut}/binder/include)
target_link_libraries(utils_binder PUBLIC cutils log base)
target_compile_definitions(utils_binder PRIVATE ${_utils_defs})
target_compile_options(utils_binder PRIVATE ${_utils_flags} -Winvalid-offsetof -Wsequence-point
  -Wzero-as-null-pointer-constant)

add_library(utils STATIC
  ${_ut}/FileMap.cpp ${_ut}/JenkinsHash.cpp ${_ut}/LightRefBase.cpp
  ${_ut}/NativeHandle.cpp ${_ut}/Printer.cpp ${_ut}/StopWatch.cpp
  ${_ut}/SystemClock.cpp ${_ut}/Threads.cpp ${_ut}/Timers.cpp ${_ut}/Tokenizer.cpp
  ${_ut}/misc.cpp
  # linux (Gesher: Windows too)
  ${_ut}/Looper.cpp)
target_include_directories(utils PUBLIC ${_ut}/include ${_core}/libsystem/include)
target_link_libraries(utils PUBLIC utils_binder cutils log base)
target_compile_definitions(utils PRIVATE ${_utils_defs})
target_compile_options(utils PRIVATE ${_utils_flags})
