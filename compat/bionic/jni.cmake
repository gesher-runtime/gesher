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

# Bitmap's NDK entry points over hwui's ABitmap storage, with the loaded
# library's System V ABI and JNI proxy translated at the boundary.
add_library(gesher_libjnigraphics OBJECT ${CMAKE_CURRENT_LIST_DIR}/libjnigraphics.cc)
target_include_directories(gesher_libjnigraphics PRIVATE ${CMAKE_CURRENT_LIST_DIR}
  ${AOSP_DIR}/frameworks/base/libs/hwui/apex/include)
target_link_libraries(gesher_libjnigraphics PRIVATE gesher_bionic hwui nativehelper)
target_compile_options(gesher_libjnigraphics PRIVATE -Wall -Wextra -Werror)
target_link_libraries(dalvikvm PRIVATE gesher_libjnigraphics)

# NDK font matching shares Typeface's Minikin collections and shipped fonts.
add_library(gesher_native_fonts OBJECT ${CMAKE_CURRENT_LIST_DIR}/native_fonts.cc)
target_include_directories(gesher_native_fonts PRIVATE ${CMAKE_CURRENT_LIST_DIR})
# Keep -Werror for the bridge while treating upstream Minikin/hwui headers
# like external headers (deprecated is_pod and missing override annotations).
target_include_directories(gesher_native_fonts SYSTEM PRIVATE
  ${AOSP_DIR}/frameworks/base/libs/hwui ${AOSP_DIR}/frameworks/minikin/include)
target_link_libraries(gesher_native_fonts PRIVATE gesher_bionic hwui tinyxml2)
set_target_properties(gesher_native_fonts PROPERTIES CXX_STANDARD 23)
target_compile_options(gesher_native_fonts PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_link_libraries(dalvikvm PRIVATE gesher_native_fonts)

# gesher: libandroid's ANativeWindow functions and EGL/GLES (ANGLE) for the
# app's .so files, and their Windows test (compat/bionic/ndk_graphics.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/ndk_graphics.cmake)

# gesher: NativeActivity, InputQueue, AInputQueue/AInputEvent and AChoreographer
# for the app's .so files (compat/bionic/native_activity.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/native_activity.cmake)
