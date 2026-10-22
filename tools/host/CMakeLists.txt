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

# --- protoc, libprotobuf, libprotoc (external/protobuf, with abseil) --------
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_CONFORMANCE OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_LIBUPB OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(protobuf_ABSL_PROVIDER module CACHE STRING "" FORCE)
set(ABSL_ROOT_DIR ${AOSP_DIR}/external/abseil-cpp CACHE PATH "" FORCE)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(${AOSP_DIR}/external/protobuf protobuf EXCLUDE_FROM_ALL)
add_custom_target(host_protoc ALL DEPENDS protoc)

set(_pn ${AOSP_DIR}/external/protobuf/src/google/protobuf/compiler/javanano)
add_executable(protoc-gen-javanano
  ${_pn}/javanano_enum.cc ${_pn}/javanano_enum_field.cc ${_pn}/javanano_extension.cc
  ${_pn}/javanano_field.cc ${_pn}/javanano_file.cc ${_pn}/javanano_generator.cc
  ${_pn}/javanano_helpers.cc ${_pn}/javanano_main.cc ${_pn}/javanano_map_field.cc
  ${_pn}/javanano_message.cc ${_pn}/javanano_message_field.cc
  ${_pn}/javanano_primitive_field.cc)
target_include_directories(protoc-gen-javanano PRIVATE ${AOSP_DIR}/external/protobuf/src)
target_link_libraries(protoc-gen-javanano PRIVATE libprotoc libprotobuf)

set(_sp ${AOSP_DIR}/frameworks/base/tools/streaming_proto)
add_executable(protoc-gen-javastream
  ${_sp}/java/main.cpp ${_sp}/java/java_proto_stream_code_generator.cpp
  ${_sp}/Errors.cpp ${_sp}/stream_proto_utils.cpp ${_sp}/string_utils.cpp)
target_include_directories(protoc-gen-javastream PRIVATE ${_sp})
target_link_libraries(protoc-gen-javastream PRIVATE libprotoc libprotobuf)

# --- Protos compiled to C++ (proto: { type: "full" }) ----------------------
# Paths are canonical from the AOSP root, as Soong's: "frameworks/.../x.pb.h".
find_package(Python3 REQUIRED COMPONENTS Interpreter)
function(bp_srcs out dir module)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/bp_srcs.py ${AOSP_DIR} ${dir} ${module}
    OUTPUT_VARIABLE _srcs RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "bp_srcs.py failed for ${module}")
  endif()
  set(${out} "${_srcs}" PARENT_SCOPE)
endfunction()

set(_pgen ${CMAKE_CURRENT_BINARY_DIR}/proto)
function(aosp_proto_library name)
  set(_srcs)
  foreach(proto ${ARGN})
    file(RELATIVE_PATH _rel ${AOSP_DIR} ${proto})
    string(REGEX REPLACE "\.proto$" "" _stem ${_rel})
    add_custom_command(
      OUTPUT ${_pgen}/${_stem}.pb.cc ${_pgen}/${_stem}.pb.h
      COMMAND ${CMAKE_COMMAND} -E make_directory ${_pgen}
      COMMAND $<TARGET_FILE:protoc> --cpp_out=${_pgen} -I${AOSP_DIR}
              -I${AOSP_DIR}/external/protobuf/src ${_rel}
      WORKING_DIRECTORY ${AOSP_DIR}
      DEPENDS protoc ${proto})
    list(APPEND _srcs ${_pgen}/${_stem}.pb.cc)
  endforeach()
  add_library(${name} STATIC ${_srcs})
  target_include_directories(${name} PUBLIC ${_pgen})
  target_link_libraries(${name} PUBLIC libprotobuf)
endfunction()

# --- stats-log-api-gen ------------------------------------------------------
set(_st ${AOSP_DIR}/frameworks/proto_logging/stats)
bp_srcs(_stats_protos frameworks/proto_logging/stats libstats_proto_host)
aosp_proto_library(host_stats_proto ${_stats_protos})
add_executable(stats-log-api-gen
  ${_st}/stats_log_api_gen/Collation.cpp ${_st}/stats_log_api_gen/settings_provider.cpp
  ${_st}/stats_log_api_gen/utils.cpp ${_st}/stats_log_api_gen/java_writer.cpp
  ${_st}/stats_log_api_gen/java_writer_q.cpp ${_st}/stats_log_api_gen/java_writer_vendor.cpp
  ${_st}/stats_log_api_gen/main.cpp ${_st}/stats_log_api_gen/native_writer.cpp
  ${_st}/stats_log_api_gen/native_writer_vendor.cpp ${_st}/stats_log_api_gen/rust_writer.cpp)
target_compile_definitions(stats-log-api-gen PRIVATE WITH_VENDOR
  JAVA_INCLUDE_SRCS_DIR="java/" CC_INCLUDE_HDRS_DIR="cc_hdrs/" CC_INCLUDE_SRCS_DIR="cc_srcs/")
target_link_libraries(stats-log-api-gen PRIVATE host_stats_proto host_base)

# --- expresscatalog-codegen -------------------------------------------------
set(_ex ${_st}/express)
aosp_proto_library(host_express_proto ${_ex}/express_config.proto)
set(GFLAGS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(GFLAGS_NAMESPACE google CACHE STRING "" FORCE)  # as AOSP's gflags is configured
add_subdirectory(${AOSP_DIR}/external/gflags gflags EXCLUDE_FROM_ALL)
add_executable(expresscatalog-codegen
  ${_ex}/expresscatalog-code-gen/codegen.cpp ${_ex}/expresscatalog-code-gen/codegen_java.cpp
  ${_ex}/expresscatalog-code-gen/main.cpp ${_ex}/libexpresscatalog/catalog.cpp
  ${AOSP_DIR}/external/libtextclassifier/native/utils/hash/farmhash.cc)
target_include_directories(expresscatalog-codegen PRIVATE
  ${_ex}/libexpresscatalog/include ${_ex}/libexpresscatalog-utils/include
  ${AOSP_DIR}/external/libtextclassifier/native)
target_compile_definitions(expresscatalog-codegen PRIVATE NAMESPACE_FOR_HASH_FUNCTIONS=farmhash)
target_link_libraries(expresscatalog-codegen PRIVATE host_express_proto host_base gflags::gflags)

# --- sysprop_java -----------------------------------------------------------
set(_sy ${AOSP_DIR}/system/tools/sysprop)
set(_pis ${AOSP_DIR}/system/core/property_service/libpropertyinfoserializer)
aosp_proto_library(host_sysprop_proto ${_sy}/sysprop.proto)
add_executable(sysprop_java ${_sy}/JavaGen.cpp ${_sy}/JavaMain.cpp ${_sy}/Common.cpp
  ${_sy}/CodeWriter.cpp
  ${_pis}/property_info_file.cpp ${_pis}/property_info_serializer.cpp
  ${_pis}/trie_builder.cpp ${_pis}/trie_serializer.cpp)
target_include_directories(sysprop_java PRIVATE ${_sy}/include ${_pis}/include
  ${AOSP_DIR}/system/core/property_service/libpropertyinfoparser/include
  ${_pgen}/system/tools/sysprop)
target_link_libraries(sysprop_java PRIVATE host_sysprop_proto host_base)

# --- hidl-gen ---------------------------------------------------------------
set(_h ${AOSP_DIR}/system/tools/hidl)
set(_js ${AOSP_DIR}/external/jsoncpp)
add_library(host_jsoncpp STATIC ${_js}/src/lib_json/json_reader.cpp
  ${_js}/src/lib_json/json_value.cpp ${_js}/src/lib_json/json_writer.cpp)
target_include_directories(host_jsoncpp PUBLIC ${_js}/include)
bison_target(hidl_parser ${_h}/hidl-gen_y.yy ${CMAKE_CURRENT_BINARY_DIR}/hidl-gen_y.cpp
  DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/hidl-gen_y.h)
flex_target(hidl_lexer ${_h}/hidl-gen_l.ll ${CMAKE_CURRENT_BINARY_DIR}/hidl-gen_l.cpp)
add_flex_bison_dependency(hidl_lexer hidl_parser)
add_executable(hidl-gen ${_h}/main.cpp
  ${_h}/Annotation.cpp ${_h}/ArrayType.cpp ${_h}/CompoundType.cpp ${_h}/ConstantExpression.cpp
  ${_h}/DeathRecipientType.cpp ${_h}/DocComment.cpp ${_h}/EnumType.cpp ${_h}/FmqType.cpp
  ${_h}/HandleType.cpp ${_h}/HidlTypeAssertion.cpp ${_h}/Interface.cpp ${_h}/Location.cpp
  ${_h}/MemoryType.cpp ${_h}/Method.cpp ${_h}/NamedType.cpp ${_h}/PointerType.cpp
  ${_h}/ScalarType.cpp ${_h}/Scope.cpp ${_h}/StringType.cpp ${_h}/Type.cpp ${_h}/TypeDef.cpp
  ${_h}/VectorType.cpp
  ${_h}/AST.cpp ${_h}/Coordinator.cpp ${_h}/generateCpp.cpp ${_h}/generateCppImpl.cpp
  ${_h}/generateDependencies.cpp ${_h}/generateFormattedHidl.cpp
  ${_h}/generateInheritanceHierarchy.cpp ${_h}/generateJava.cpp ${_h}/generateJavaImpl.cpp
  ${_h}/generateVts.cpp
  ${_h}/hashing/Hash.cpp ${_h}/host_utils/Formatter.cpp ${_h}/host_utils/StringHelper.cpp
  ${_h}/utils/FQName.cpp ${_h}/utils/FqInstance.cpp
  ${BISON_hidl_parser_OUTPUTS} ${FLEX_hidl_lexer_OUTPUTS})
target_include_directories(hidl-gen PRIVATE ${_h} ${CMAKE_CURRENT_BINARY_DIR}
  ${_h}/hashing/include ${_h}/host_utils/include ${_h}/host_utils/include/hidl-util
  ${_h}/utils/include ${_h}/utils/include/hidl-util
  ${AOSP_DIR}/system/libhwbinder/include ${AOSP_DIR}/system/core/libutils/include
  ${AOSP_DIR}/system/core/libutils/binder/include ${CMAKE_CURRENT_SOURCE_DIR}/shim)
target_link_libraries(hidl-gen PRIVATE host_base host_jsoncpp)
