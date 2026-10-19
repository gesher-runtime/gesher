# ART runtime (libart-runtime) and the libraries it needs, mirroring
# art/runtime/Android.bp. Phase 1 work in progress: the runtime is compiled
# file by file and tracked by tools/compile-report.py; it is not linked yet.

set(_rt "${_art}/runtime")
set(_gen "${CMAKE_BINARY_DIR}/gen")
include(cmake/aosp/libart_srcs.cmake)

# --- aconfig flags (cc_aconfig_library art-aconfig-flags-lib, -rw-) ---------
set(_aconfig_values "${CMAKE_SOURCE_DIR}/config/aconfig/art.values")
foreach(pair "art-flags.aconfig:com_android_art_flags.h"
             "art-rw-flags.aconfig:com_android_art_rw_flags.h")
  string(REPLACE ":" ";" pair "${pair}")
  list(GET pair 0 decl)
  list(GET pair 1 header)
  add_custom_command(
    OUTPUT "${_gen}/aconfig/${header}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen}/aconfig"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen/aconfig_header.py"
            "${_art}/build/flags/${decl}" "${_aconfig_values}" > "${_gen}/aconfig/${header}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen/aconfig_header.py" "${_art}/build/flags/${decl}"
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

list(TRANSFORM LIBART_RUNTIME_SRCS PREPEND "${_rt}/")
add_library(art_runtime STATIC ${LIBART_RUNTIME_SRCS} ${runtime_ops})
target_compile_definitions(art_runtime PRIVATE BUILDING_LIBART)
target_compile_options(art_runtime PRIVATE -fno-delete-null-pointer-checks)
target_link_libraries(art_runtime PUBLIC art_runtime_headers)
add_dependencies(art_runtime art_asm_support_headers)
# Not part of `all` until it compiles: build with `ninja -k 0 art_runtime`.
set_target_properties(art_runtime PROPERTIES EXCLUDE_FROM_ALL ON)
