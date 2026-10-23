# gesher: JNI for the app's .so files (compat/bionic/jni_bridge.cc), linked
# into dalvikvm. An OBJECT library because nothing calls it: its static
# initializer registers the ELF loader with gesher-base's dlopen
# (<gesher/elf_dlopen.h>), which ART's host libnativeloader path calls for
# System.loadLibrary. Phase 6, docs/native-libs.md.
add_library(gesher_jni_bridge OBJECT ${CMAKE_CURRENT_LIST_DIR}/jni_bridge.cc)
target_include_directories(gesher_jni_bridge PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(gesher_jni_bridge PRIVATE gesher_bionic gesher_elf nativehelper)
target_compile_options(gesher_jni_bridge PRIVATE -Wall -Wextra -Werror)
target_link_libraries(dalvikvm PRIVATE gesher_jni_bridge gesher_bionic gesher_elf)

# gesher: libandroid.so's non-graphics NDK APIs for the app's .so files
# (compat/bionic/libandroid.cc), over libandroidfw, the framework's
# AssetManager natives and libutils' Looper; its static initializer adds them
# to the bionic shim's symbol table. Linked into dalvikvm next to the
# framework's natives (framework_runtime). docs/native-libs.md.
add_library(gesher_libandroid OBJECT ${CMAKE_CURRENT_LIST_DIR}/libandroid.cc)
target_include_directories(gesher_libandroid PRIVATE ${CMAKE_CURRENT_LIST_DIR}
  ${AOSP_DIR}/frameworks/base/core/jni/include)
target_link_libraries(gesher_libandroid PRIVATE gesher_bionic gesher_elf androidfw utils cutils log
  base nativehelper)
# As framework_runtime: C++23 (utils/Trace.h and <stdatomic.h>), and
# ResourceTypes.h's anonymous enum arithmetic.
set_target_properties(gesher_libandroid PROPERTIES CXX_STANDARD 23)
target_compile_options(gesher_libandroid PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter
  -Wno-deprecated-anon-enum-enum-conversion -Wno-deprecated-copy)
target_link_libraries(dalvikvm PRIVATE gesher_libandroid)
