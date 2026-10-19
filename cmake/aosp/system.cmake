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
