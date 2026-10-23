# external/googletest: libgtest, libgtest_main and libgmock, mirroring their
# Android.bp (Windows enabled), for AOSP's own unit tests of the core
# libraries (cmake/aosp/core_libs_tests.cmake). Built only for the tests.

set(_gt "${AOSP_DIR}/external/googletest/googletest")
set(_gm "${AOSP_DIR}/external/googletest/googlemock")

# libgtest_defaults: -Wall -Werror -Wno-unused-private-field; rtti.
add_library(gtest STATIC EXCLUDE_FROM_ALL ${_gt}/src/gtest-all.cc)
target_include_directories(gtest PUBLIC ${_gt}/include PRIVATE ${_gt})
target_compile_options(gtest PRIVATE -Wall -Werror -Wno-unused-private-field)

add_library(gtest_main STATIC EXCLUDE_FROM_ALL ${_gt}/src/gtest_main.cc)
target_link_libraries(gtest_main PUBLIC gtest)
target_compile_options(gtest_main PRIVATE -Wall -Werror -Wno-unused-private-field)

# gmock_flags: -Wall -Werror -Wno-missing-field-initializers -Wno-sign-compare
# -Wno-unused-parameter -fexceptions; rtti.
add_library(gmock STATIC EXCLUDE_FROM_ALL ${_gm}/src/gmock-all.cc)
target_include_directories(gmock PUBLIC ${_gm}/include PRIVATE ${_gm})
target_link_libraries(gmock PUBLIC gtest)
target_compile_options(gmock PRIVATE -Wall -Werror -Wno-missing-field-initializers
  -Wno-sign-compare -Wno-unused-parameter -fexceptions)
