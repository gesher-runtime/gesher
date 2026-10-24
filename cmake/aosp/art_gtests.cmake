# ART's own gtests for libartbase and libdexfile (art_libartbase_tests,
# art_libdexfile_tests in art/libartbase/Android.bp and art/libdexfile/Android.bp),
# built with googletest (cmake/aosp/googletest.cmake) and run on Windows by
# tests/windows/tracks/21-posix.ps1. Not part of the default build: `ninja
# art_gtests` builds them into art-gtests/ (.github/workflows/posix.yml, which
# also builds the test jars they read). Every source the test modules list is
# built; tests that cannot run on Windows are excluded by name in 21-posix.ps1,
# with the reason in docs/posix.md.

set(_art_gtests_dir "${CMAKE_BINARY_DIR}/art-gtests")
add_custom_target(art_gtests)

# libartbase-testing + libartbase-art-gtest (common_art_test.cc) + libart-gtest's
# main. ART's main (art/test/common/gtest_main.cc) runs every test in a forked
# child (gtest_extras' IsolateMain); Gesher's runs them in one process, after the
# same initialization.
add_library(art_gtest_common STATIC EXCLUDE_FROM_ALL
  ${_art}/libartbase/base/testing.cc
  ${_art}/libartbase/base/common_art_test.cc
  # libart-runtime-gtest's CommonRuntimeTest (flags_test.cc's fixtures).
  ${_art}/runtime/common_runtime_test.cc
  ${CMAKE_SOURCE_DIR}/tests/art-gtests/art_gtest_main.cc)
target_include_directories(art_gtest_common PUBLIC
  ${AOSP_DIR}/libnativehelper/header_only_include
  ${AOSP_DIR}/libnativehelper/include_jni)
# art_test_common_defaults' cflags.
target_compile_options(art_gtest_common PUBLIC
  -Wno-used-but-marked-unused -Wno-sign-compare -Wno-deprecated-dynamic-exception-spec
  -Wno-frame-larger-than= -Wno-unused-command-line-argument -Wno-deprecated)
target_link_libraries(art_gtest_common PUBLIC art_runtime art_compiler gmock gtest)

# art_gtest(<name> SOURCES <...>)
function(art_gtest name)
  cmake_parse_arguments(T "" "" "SOURCES" ${ARGN})
  add_executable(${name} EXCLUDE_FROM_ALL ${T_SOURCES}
    # libart-compiler's graph visualizer asks for a disassembler (as dalvikvm).
    ${CMAKE_SOURCE_DIR}/compat/art/disassembler_standin.cc)
  target_link_libraries(${name} PRIVATE art_gtest_common)
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${_art_gtests_dir})
  add_dependencies(art_gtests ${name})
endfunction()

# --- art_libartbase_tests (art_libartbase_tests_defaults) ---------------------
set(_abt "${_art}/libartbase")
# Not built: base/aconfig_flags_test.cc static_asserts the release config's
# test-flag values (art `test`, libcore `v_apis` true), while Gesher's flags
# come from config/aconfig/art.values, where every unlisted flag is false.
art_gtest(art_libartbase_tests SOURCES
  ${_abt}/arch/instruction_set_test.cc
  ${_abt}/base/arena_allocator_test.cc
  ${_abt}/base/bit_field_test.cc
  ${_abt}/base/bit_memory_region_test.cc
  ${_abt}/base/bit_string_test.cc
  ${_abt}/base/bit_struct_test.cc
  ${_abt}/base/bit_table_test.cc
  ${_abt}/base/bit_utils_test.cc
  ${_abt}/base/bit_vector_test.cc
  ${_abt}/base/compiler_filter_test.cc
  ${_abt}/base/file_utils_test.cc
  ${_abt}/base/flags_test.cc
  ${_abt}/base/hash_map_test.cc
  ${_abt}/base/hash_set_test.cc
  ${_abt}/base/hex_dump_test.cc
  ${_abt}/base/histogram_test.cc
  ${_abt}/base/indenter_test.cc
  ${_abt}/base/intrusive_forward_list_test.cc
  ${_abt}/base/leb128_test.cc
  ${_abt}/base/logging_test.cc
  ${_abt}/base/mem_map_test.cc
  ${_abt}/base/membarrier_test.cc
  ${_abt}/base/memfd_test.cc
  ${_abt}/base/memory_region_test.cc
  ${_abt}/base/metrics/metrics_test.cc
  ${_abt}/base/scoped_flock_test.cc
  ${_abt}/base/time_utils_test.cc
  ${_abt}/base/transform_array_ref_test.cc
  ${_abt}/base/transform_iterator_test.cc
  ${_abt}/base/unix_file/fd_file_test.cc
  ${_abt}/base/utils_test.cc
  ${_abt}/base/variant_map_test.cc
  ${_abt}/base/zip_archive_test.cc)

# --- art_libdexfile_tests (art_libdexfile_tests_defaults) ---------------------
set(_dxt "${_art}/libdexfile/dex")
art_gtest(art_libdexfile_tests SOURCES
  ${_dxt}/art_dex_file_loader_test.cc
  ${_dxt}/class_accessor_test.cc
  ${_dxt}/code_item_accessors_test.cc
  ${_dxt}/descriptors_names_test.cc
  ${_dxt}/dex_file_loader_test.cc
  ${_dxt}/dex_file_verifier_test.cc
  ${_dxt}/dex_instruction_test.cc
  ${_dxt}/primitive_test.cc
  ${_dxt}/proto_reference_test.cc
  ${_dxt}/string_reference_test.cc
  ${_dxt}/test_dex_file_builder_test.cc
  ${_dxt}/type_lookup_table_test.cc
  ${_dxt}/utf_test.cc)
