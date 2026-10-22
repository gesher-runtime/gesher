# Build tools Gesher's framework build runs on the Linux build host, built
# from the AOSP sources at the pinned tag, as Soong builds them for the host:
# aidl, protoc with Android's javanano and javastream plugins,
# stats-log-api-gen, sysprop_java and hidl-gen.
#
#   cmake -S tools/host -B out/host -G Ninja -DAOSP_DIR=<aosp> && ninja -C out/host
#
# Needs a C++17 compiler, bison and flex. Not part of the Windows build.
cmake_minimum_required(VERSION 3.20)
project(gesher_host_tools C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT AOSP_DIR)
  set(AOSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/aosp")
endif()
get_filename_component(AOSP_DIR "${AOSP_DIR}" ABSOLUTE)
add_compile_options(-w)

find_package(BISON 3 REQUIRED)
find_package(FLEX REQUIRED)

# --- libbase, liblog (host) ------------------------------------------------
set(_b ${AOSP_DIR}/system/libbase)
add_library(host_base STATIC
  ${_b}/chrono_utils.cpp ${_b}/cmsg.cpp ${_b}/file.cpp ${_b}/hex.cpp
  ${_b}/logging.cpp ${_b}/mapped_file.cpp ${_b}/parsebool.cpp
  ${_b}/parsenetaddress.cpp ${_b}/posix_strerror_r.cpp ${_b}/process.cpp
  ${_b}/properties.cpp ${_b}/result.cpp ${_b}/stringprintf.cpp
  ${_b}/strings.cpp ${_b}/threads.cpp ${_b}/test_utils.cpp ${_b}/errors_unix.cpp)
target_include_directories(host_base PUBLIC ${_b}/include
  ${AOSP_DIR}/system/logging/liblog/include ${AOSP_DIR}/external/fmtlib/include)
target_compile_definitions(host_base PUBLIC FMT_HEADER_ONLY)

set(_l ${AOSP_DIR}/system/logging/liblog)
add_library(host_log STATIC
  ${_l}/log_event_list.cpp ${_l}/log_event_write.cpp ${_l}/logger_name.cpp
  ${_l}/logger_read.cpp ${_l}/logger_write.cpp ${_l}/logprint.cpp ${_l}/properties.cpp)
target_include_directories(host_log PUBLIC ${_l}/include PRIVATE ${_b}/include
  ${AOSP_DIR}/system/core/libcutils/include)
target_compile_definitions(host_log PRIVATE LIBLOG_LOG_TAG=1006 SNET_EVENT_LOG_TAG=1397638484)
target_link_libraries(host_base PUBLIC host_log)
find_package(Threads REQUIRED)
target_link_libraries(host_base PUBLIC Threads::Threads)

# googletest, which aidl links as Soong's aidl_defaults do (FRIEND_TEST and
# a few of its helpers).
set(_gt ${AOSP_DIR}/external/googletest/googletest)
add_library(host_gtest STATIC ${_gt}/src/gtest-all.cc)
target_include_directories(host_gtest PUBLIC ${_gt}/include PRIVATE ${_gt})
target_link_libraries(host_gtest PUBLIC Threads::Threads)

# --- aidl ------------------------------------------------------------------
set(_a ${AOSP_DIR}/system/tools/aidl)
bison_target(aidl_parser ${_a}/aidl_language_y.yy ${CMAKE_CURRENT_BINARY_DIR}/aidl_language_y.cpp
  DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/aidl_language_y.h)
flex_target(aidl_lexer ${_a}/aidl_language_l.ll ${CMAKE_CURRENT_BINARY_DIR}/aidl_language_l.cpp)
add_flex_bison_dependency(aidl_lexer aidl_parser)
add_executable(aidl
  ${_a}/main.cpp
  ${_a}/aidl_checkapi.cpp ${_a}/aidl_const_expressions.cpp ${_a}/aidl_dumpapi.cpp
  ${_a}/aidl_language.cpp ${_a}/aidl_to_common.cpp ${_a}/aidl_to_cpp_common.cpp
  ${_a}/aidl_to_cpp.cpp ${_a}/aidl_to_java.cpp ${_a}/aidl_to_ndk.cpp
  ${_a}/aidl_to_rust.cpp ${_a}/aidl_typenames.cpp ${_a}/aidl.cpp ${_a}/ast_java.cpp
  ${_a}/check_valid.cpp ${_a}/code_writer.cpp ${_a}/comments.cpp ${_a}/diagnostics.cpp
  ${_a}/generate_aidl_mappings.cpp ${_a}/generate_cpp.cpp ${_a}/generate_cpp_analyzer.cpp
  ${_a}/generate_java_binder.cpp ${_a}/generate_java.cpp ${_a}/generate_ndk.cpp
  ${_a}/generate_rust.cpp ${_a}/import_resolver.cpp ${_a}/io_delegate.cpp
  ${_a}/location.cpp ${_a}/logging.cpp ${_a}/options.cpp ${_a}/parser.cpp
  ${_a}/permission.cpp ${_a}/preprocess.cpp
  ${BISON_aidl_parser_OUTPUTS} ${FLEX_aidl_lexer_OUTPUTS})
target_include_directories(aidl PRIVATE ${_a} ${_a}/include ${CMAKE_CURRENT_BINARY_DIR})
# Soong's platform_sdk_version for the pinned release.
target_compile_definitions(aidl PRIVATE PLATFORM_SDK_VERSION=36)
target_link_libraries(aidl PRIVATE host_base host_gtest)
