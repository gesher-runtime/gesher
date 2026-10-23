# AOSP's own unit tests of libutils, libcutils and libandroidfw, built from
# their Android.bp cc_test modules with googletest (cmake/aosp/googletest.cmake)
# and run on Windows by tests/windows/tracks/20-libs.ps1. Not part of the
# default build: `ninja core_libs_tests` builds them into core-libs-tests/,
# with libandroidfw's test data (.github/workflows/libs.yml). Sources a test
# module lists for the device are built too; the tests Windows cannot run are
# listed, with the reason, in docs/core-libs.md and excluded there by name.

set(_ut "${AOSP_DIR}/system/core/libutils")
set(_cu "${AOSP_DIR}/system/core/libcutils")
set(_afw "${AOSP_DIR}/frameworks/base/libs/androidfw")
set(_core_tests_dir "${CMAKE_BINARY_DIR}/core-libs-tests")

add_custom_target(core_libs_tests)

# gesher_core_test(<name> FLAGS <...> SOURCES <...> LIBS <...>)
function(gesher_core_test name)
  cmake_parse_arguments(T "" "" "FLAGS;SOURCES;LIBS;DEFINES" ${ARGN})
  add_executable(${name} EXCLUDE_FROM_ALL ${T_SOURCES})
  target_link_libraries(${name} PRIVATE ${T_LIBS})
  target_compile_options(${name} PRIVATE ${T_FLAGS})
  target_compile_definitions(${name} PRIVATE ${T_DEFINES})
  # Unstripped, unlike the tools: CallStack's tests read the symbol table.
  set_target_properties(${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${_core_tests_dir}
    LINK_OPTIONS "-static")
  add_dependencies(core_libs_tests ${name})
endfunction()

# --- libutils_binder_test, libutils_binder_test_custom_allocator ----------------
# libutils_binder_test_defaults_no_libutils, with RefBase_test.cpp (linux).
set(_ub_test_srcs
  ${_ut}/binder/Errors_test.cpp ${_ut}/binder/SharedBuffer_test.cpp
  ${_ut}/binder/String16_test.cpp ${_ut}/binder/String8_test.cpp
  ${_ut}/binder/StrongPointer_test.cpp ${_ut}/binder/Unicode_test.cpp
  ${_ut}/binder/Vector_test.cpp ${_ut}/binder/RefBase_test.cpp)
set(_ut_test_flags -Wall -Wextra -Wthread-safety)
gesher_core_test(libutils_binder_test
  SOURCES ${_ub_test_srcs}
  FLAGS ${_ut_test_flags}
  LIBS utils_binder base log lzma gmock gtest_main)
# Errors_test.cpp includes utils/ErrorsMacros.h, a libutils header.
target_include_directories(libutils_binder_test PRIVATE ${_ut}/include)
gesher_core_test(libutils_binder_test_custom_allocator
  SOURCES ${_ub_test_srcs}
  FLAGS ${_ut_test_flags}
  DEFINES ANDROID_UTILS_CUSTOM_ALLOCATOR
  LIBS utils_binder_custom_allocator base log lzma gmock gtest_main)
target_include_directories(libutils_binder_test_custom_allocator PRIVATE ${_ut}/include)

# --- libutils_test --------------------------------------------------------------
# With SystemClock_test.cpp (android) and Looper_test.cpp (linux).
gesher_core_test(libutils_test
  SOURCES
    ${_ut}/BitSet_test.cpp ${_ut}/CallStack_test.cpp ${_ut}/FileMap_test.cpp
    ${_ut}/LruCache_test.cpp
    ${_ut}/Mutex_test.cpp ${_ut}/Singleton_test.cpp ${_ut}/Timers_test.cpp
    ${_ut}/SystemClock_test.cpp ${_ut}/Looper_test.cpp
  FLAGS ${_ut_test_flags}
  LIBS utils utilscallstack base log lzma z gtest_main)

# libutils_test_singleton1 and 2, which Singleton_test loads from the test's
# directory by their Android names: DLLs named *.so. The second takes the
# singleton's instance from the first (its import), as the dynamic linker
# resolves it on Android.
foreach(_n 1 2)
  set(_t libutils_test_singleton${_n})
  add_library(${_t} SHARED EXCLUDE_FROM_ALL ${_ut}/Singleton_test${_n}.cpp)
  target_include_directories(${_t} PRIVATE
    $<TARGET_PROPERTY:utils,INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_options(${_t} PRIVATE -Wall)
  set_target_properties(${_t} PROPERTIES PREFIX "" SUFFIX ".so"
    RUNTIME_OUTPUT_DIRECTORY ${_core_tests_dir}
    LINK_OPTIONS "-static;-Wl,--export-all-symbols")
  add_dependencies(core_libs_tests ${_t})
endforeach()
target_link_libraries(libutils_test_singleton2 PRIVATE libutils_test_singleton1)

# --- libcutils_test -------------------------------------------------------------
# libcutils_test_default with its android and not_windows sources, less
# sched_policy_test.cpp, which tests libprocessgroup's cgroup policies, and
# ashmem_test.cpp, which tests the device's ashmem-dev.cpp through the ashmem
# driver's ioctls and fork() (docs/core-libs.md). trace-dev_test.cpp builds the device's trace-dev.cpp
# into itself, with C's <stdatomic.h>, which libc++ allows next to <atomic>
# from C++23 on.
gesher_core_test(libcutils_test
  SOURCES
    ${_cu}/ashmem_base_test.cpp ${_cu}/native_handle_test.cpp ${_cu}/properties_test.cpp
    ${_cu}/sockets_test.cpp
    # android
    ${_cu}/android_get_control_file_test.cpp ${_cu}/android_get_control_socket_test.cpp
    ${_cu}/fs_config_test.cpp ${_cu}/multiuser_test.cpp
    ${_cu}/str_parms_test.cpp ${_cu}/trace-dev_test.cpp
  FLAGS -Wall -Wextra
  LIBS cutils log base gtest_main)
target_include_directories(libcutils_test PRIVATE ${_cu})
set_target_properties(libcutils_test PROPERTIES CXX_STANDARD 23)
# cutils/properties.h sizes keys with bionic's PROP_NAME_MAX, which only
# bionic's <sys/system_properties.h> defines; trace-dev.inc uses it.
target_compile_definitions(libcutils_test PRIVATE PROP_NAME_MAX=32)

# --- libandroidfw_tests ---------------------------------------------------------
# With the android sources (BackupData, BackupHelpers, ObbFile), less
# PosixUtils_test.cpp: libandroidfw's PosixUtils.cpp compiles
# android::util::ExecuteBinary out on _WIN32 (fork/execvp; its one user,
# OverlayConfig's JNI running /system/bin/idmap2, is not built), and the test
# runs /bin/date (docs/core-libs.md).
gesher_core_test(libandroidfw_tests
  SOURCES
    ${_afw}/tests/CommonHelpers.cpp ${_afw}/tests/TestHelpers.cpp ${_afw}/tests/TestMain.cpp
    ${_afw}/tests/ApkAssets_test.cpp ${_afw}/tests/ApkParsing_test.cpp
    ${_afw}/tests/AppAsLib_test.cpp ${_afw}/tests/Asset_test.cpp
    ${_afw}/tests/AssetManager2_test.cpp ${_afw}/tests/AttributeFinder_test.cpp
    ${_afw}/tests/AttributeResolution_test.cpp ${_afw}/tests/BigBuffer_test.cpp
    ${_afw}/tests/ByteBucketArray_test.cpp ${_afw}/tests/CombinedIterator_test.cpp
    ${_afw}/tests/Config_test.cpp ${_afw}/tests/ConfigDescription_test.cpp
    ${_afw}/tests/ConfigLocale_test.cpp ${_afw}/tests/CursorWindow_test.cpp
    ${_afw}/tests/DynamicRefTable_test.cpp ${_afw}/tests/FileStream_test.cpp
    ${_afw}/tests/Idmap_test.cpp ${_afw}/tests/LoadedArsc_test.cpp
    ${_afw}/tests/Locale_test.cpp ${_afw}/tests/LocaleDataLookup_test.cpp
    ${_afw}/tests/NinePatch_test.cpp ${_afw}/tests/ResourceTimer_test.cpp
    ${_afw}/tests/ResourceUtils_test.cpp ${_afw}/tests/ResTable_test.cpp
    ${_afw}/tests/sorted_vector_set_test.cpp ${_afw}/tests/Split_test.cpp
    ${_afw}/tests/StringPiece_test.cpp ${_afw}/tests/StringPool_test.cpp
    ${_afw}/tests/Theme_test.cpp ${_afw}/tests/TypeWrappers_test.cpp
    ${_afw}/tests/ZipUtils_test.cpp
    # android
    ${_afw}/tests/BackupData_test.cpp ${_afw}/tests/BackupHelpers_test.cpp
    ${_afw}/tests/ObbFile_test.cpp
  # libandroidfw_defaults, and -Wno-unnamed-type-template-args for gtest.
  FLAGS -Wunreachable-code -Wno-missing-field-initializers -Wno-unnamed-type-template-args
  LIBS androidfw binder ziparchive utils cutils base log z gmock)
target_include_directories(libandroidfw_tests PRIVATE ${_afw}/tests
  ${CMAKE_SOURCE_DIR}/compat/framework/include)
target_compile_definitions(libandroidfw_tests PRIVATE STATIC_ANDROIDFW_FOR_TOOLS)
set_target_properties(libandroidfw_tests PROPERTIES CXX_STANDARD 23)
# The test data (tests/data/**/*.apk, *.arsc, *.idmap) next to the executable.
add_custom_command(TARGET libandroidfw_tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${_afw}/tests/data ${_core_tests_dir}/tests/data)
