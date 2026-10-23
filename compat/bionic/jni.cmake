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
