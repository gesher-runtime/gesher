# RFC 7932 decoder from the same pinned AOSP release as the HTTP provider.
# Retain the upstream MIT license in the source checkout and distribution
# inventory. The encoder and nonstandard large-window feature are not enabled.
set(_br "${AOSP_DIR}/external/brotli")
file(GLOB _br_common CONFIGURE_DEPENDS "${_br}/c/common/*.c")
file(GLOB _br_decode CONFIGURE_DEPENDS "${_br}/c/dec/*.c")
add_library(gesher_brotli STATIC ${_br_common} ${_br_decode})
target_include_directories(gesher_brotli PUBLIC "${_br}/c/include")
add_library(gesher_brotli_jni OBJECT "${CMAKE_SOURCE_DIR}/compat/net/brotli_jni.cc")
target_compile_options(gesher_brotli_jni PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(gesher_brotli_jni PUBLIC nativehelper gesher_brotli)
target_link_libraries(dalvikvm PRIVATE gesher_brotli_jni)
