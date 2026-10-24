# ART runtime (libart-runtime) and the libraries it needs, mirroring
# art/runtime/Android.bp. Phase 1 work in progress: the runtime is compiled
# file by file and tracked by tools/compile-report.py; it is not linked yet.

set(_rt "${_art}/runtime")
set(_gen "${CMAKE_BINARY_DIR}/gen")
include(cmake/aosp/libart_srcs.cmake)

# --- aconfig flags (cc_aconfig_library art-aconfig-flags-lib, -rw-) ---------
set(_aconfig_values "${CMAKE_SOURCE_DIR}/config/aconfig/art.values")
foreach(pair "art/build/flags/art-flags.aconfig|com_android_art_flags.h"
             "art/build/flags/art-rw-flags.aconfig|com_android_art_rw_flags.h"
             "libcore/libcore.aconfig|com_android_libcore.h")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 decl)
  list(GET pair 1 header)
  add_custom_command(
    OUTPUT "${_gen}/aconfig/${header}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen}/aconfig"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen/aconfig_header.py"
            "${AOSP_DIR}/${decl}" "${_aconfig_values}" > "${_gen}/aconfig/${header}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen/aconfig_header.py" "${AOSP_DIR}/${decl}"
            "${_aconfig_values}"
    VERBATIM)
  list(APPEND _aconfig_headers "${_gen}/aconfig/${header}")
endforeach()
add_custom_target(art_aconfig_headers DEPENDS ${_aconfig_headers})

# --- headers shared by the runtime and its generators -----------------------
add_library(art_runtime_headers INTERFACE)
target_include_directories(art_runtime_headers INTERFACE
  ${_rt}
  ${_gen}/aconfig
  ${_gen}/asm_support
  ${_art}/cmdline
  ${_art}/compiler/export
  ${_art}/libelffile
  ${_art}/libprofile
  ${_art}/sigchainlib
  ${_art}/odrefresh/include
  ${_art}/libnativebridge/include
  ${_art}/libnativeloader/include
  ${_art}/tools/cpp-define-generator
  ${AOSP_DIR}/libnativehelper/header_only_include
  ${AOSP_DIR}/libnativehelper/include_platform_header_only
  ${AOSP_DIR}/external/dlmalloc
  ${AOSP_DIR}/external/lz4/lib
  ${AOSP_DIR}/external/cpu_features/include
  ${AOSP_DIR}/system/unwinding/libunwindstack/include
  ${AOSP_DIR}/system/core/libcutils/include)
target_link_libraries(art_runtime_headers INTERFACE dexfile gesher_posix)
add_dependencies(art_runtime_headers art_aconfig_headers)

# --- asm_defines.h (cpp-define-generator-asm-support) -----------------------
# Compile asm_defines.cc to assembly for the *Windows* target, so offsets and
# sizes match the LLP64 layout, then extract the constants.
add_library(art_asm_defines OBJECT ${_art}/tools/cpp-define-generator/asm_defines.cc)
target_compile_options(art_asm_defines PRIVATE -S -Wno-unused-command-line-argument)
target_link_libraries(art_asm_defines PRIVATE art_runtime_headers)
set_target_properties(art_asm_defines PROPERTIES EXCLUDE_FROM_ALL ON)
add_custom_command(
  OUTPUT "${_gen}/asm_support/asm_defines.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen}/asm_support"
  COMMAND ${Python3_EXECUTABLE} "${_art}/tools/cpp-define-generator/make_header.py"
          "$<TARGET_OBJECTS:art_asm_defines>" > "${_gen}/asm_support/asm_defines.h"
  DEPENDS art_asm_defines $<TARGET_OBJECTS:art_asm_defines>
  VERBATIM)
add_custom_target(art_asm_support_headers DEPENDS "${_gen}/asm_support/asm_defines.h")

# --- libart-runtime ---------------------------------------------------------
art_operator_srcs(runtime_ops art/runtime
  base/callee_save_type.h base/locks.h class_status.h compilation_kind.h
  gc/allocator/rosalloc.h gc/allocator_type.h gc/collector/gc_type.h
  gc/collector/mark_compact.h gc/collector_type.h gc/space/region_space.h
  gc/space/space.h gc/weak_root_state.h gc_root.h instrumentation.h
  jdwp_provider.h jni/indirect_reference_table.h jni_id_type.h linear_alloc.h
  lock_word.h oat/image.h oat/oat.h oat/oat_file.h process_state.h
  reflective_value_visitor.h stack.h suspend_reason.h thread.h thread_state.h
  trace.h trace_profile.h verifier/verifier_enums.h)

# nterp's assembly (genrule libart_mterp.x86_64ng).
file(GLOB _nterp_in "${_rt}/interpreter/mterp/x86_64ng/*.S")
list(SORT _nterp_in)
set(_nterp "${_gen}/mterp/mterp_x86_64ng.S")
add_custom_command(
  OUTPUT "${_nterp}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen}/mterp"
  COMMAND ${Python3_EXECUTABLE} "${_rt}/interpreter/mterp/gen_mterp.py" "${_nterp}" ${_nterp_in}
  DEPENDS ${_nterp_in} "${_rt}/interpreter/mterp/gen_mterp.py"
          "${_rt}/interpreter/mterp/common/gen_setup.py"
          "${_art}/libdexfile/dex/dex_instruction_list.h"
  VERBATIM)

list(TRANSFORM LIBART_RUNTIME_SRCS PREPEND "${_rt}/")
list(TRANSFORM LIBART_RUNTIME_ASM_SRCS PREPEND "${_rt}/")
add_library(art_runtime STATIC ${LIBART_RUNTIME_SRCS} ${runtime_ops}
  ${LIBART_RUNTIME_ASM_SRCS} "${_nterp}")
target_compile_definitions(art_runtime PRIVATE BUILDING_LIBART)
target_compile_options(art_runtime PRIVATE -fno-delete-null-pointer-checks)
target_link_libraries(art_runtime PUBLIC art_runtime_headers)
add_dependencies(art_runtime art_asm_support_headers)

# --- libart-runtime's static dependencies -----------------------------------
# libelffile (art/libelffile/Android.bp)
add_library(art_elffile STATIC
  ${_art}/libelffile/elf/xz_utils.cc
  ${_art}/libelffile/stream/buffered_output_stream.cc
  ${_art}/libelffile/stream/file_output_stream.cc
  ${_art}/libelffile/stream/output_stream.cc
  ${_art}/libelffile/stream/vector_output_stream.cc)
target_include_directories(art_elffile PUBLIC ${_art}/libelffile)
target_link_libraries(art_elffile PUBLIC artbase lzma)

# libprofile (art/libprofile/Android.bp)
add_library(art_profile STATIC
  ${_art}/libprofile/profile/profile_boot_info.cc
  ${_art}/libprofile/profile/profile_compilation_info.cc)
target_include_directories(art_profile PUBLIC ${_art}/libprofile)
target_link_libraries(art_profile PUBLIC dexfile)

# libsigchain: the real signal chain (Linux configuration), chaining to
# gesher-base's sigaction/sigprocmask (ART patch: lookup_libc_symbol).
add_library(art_sigchain STATIC ${_art}/sigchainlib/sigchain.cc)
target_include_directories(art_sigchain PUBLIC ${_art}/sigchainlib)
target_link_libraries(art_sigchain PUBLIC art_defaults base log gesher_posix)

# libnativebridge and libnativeloader (host configurations).
add_library(art_nativebridge STATIC ${_art}/libnativebridge/native_bridge.cc)
target_include_directories(art_nativebridge PUBLIC ${_art}/libnativebridge/include)
target_link_libraries(art_nativebridge PUBLIC art_runtime_headers)
add_library(art_nativeloader STATIC ${_art}/libnativeloader/native_loader.cpp)
target_include_directories(art_nativeloader PUBLIC ${_art}/libnativeloader/include)
target_link_libraries(art_nativeloader PUBLIC art_nativebridge art_runtime_headers)

# libodrstatslog (host: no stats are uploaded).
add_library(art_odrstatslog STATIC ${_art}/odrefresh/odr_statslog_host.cc)
target_include_directories(art_odrstatslog PUBLIC ${_art}/odrefresh/include)
target_link_libraries(art_odrstatslog PUBLIC art_defaults)

# Gesher stand-ins for modules not built (yet): libunwindstack, the aconfig
# rw-flag accessor (see compat/art/).
add_library(art_runtime_glue STATIC
  ${CMAKE_SOURCE_DIR}/compat/art/runtime_glue.cc
  ${CMAKE_SOURCE_DIR}/compat/art/unwindstack_stub.cc)
target_link_libraries(art_runtime_glue PUBLIC art_runtime_headers)

set(ART_RUNTIME_DEPS art_elffile art_profile art_sigchain art_nativebridge
  art_nativeloader art_odrstatslog art_runtime_glue cpu_features lz4 lzma)
target_link_libraries(art_runtime PUBLIC ${ART_RUNTIME_DEPS})

# --- dalvikvm (art/dalvikvm/Android.bp) -------------------------------------
add_executable(dalvikvm ${_art}/dalvikvm/dalvikvm.cc
  # Gesher: in place of libnativehelper's dlopen-based JniInvocation.
  ${CMAKE_SOURCE_DIR}/compat/art/jni_invocation_static.cc)
target_include_directories(dalvikvm PRIVATE ${AOSP_DIR}/libnativehelper/include_platform
  ${AOSP_DIR}/libnativehelper/include)
target_link_libraries(dalvikvm PRIVATE art_runtime)
# dalvikvm links libart-compiler for the JIT (cmake/aosp/dex2oat.cmake,
# docs/compiled-code.md).
# A linker map, for tools/symbolize.py (the executable is stripped).
target_link_options(dalvikvm PRIVATE -Wl,-Map,${CMAKE_BINARY_DIR}/dalvikvm.map)
