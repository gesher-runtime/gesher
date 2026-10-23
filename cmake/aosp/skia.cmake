# Skia (external/skia) for Windows, CPU raster, with the libraries it decodes
# and draws text with: FreeType, libpng, libjpeg-turbo, libwebp, wuffs.
# Source lists and flags are those of each Android.bp's Windows variant
# (tools/gen/bp_cc.py); see docs/graphics.md for what is left out and why.
#
# Third-party code is built as its project builds it, without -Werror and
# without warnings (QUIET), and without debug information: the graphics
# libraries are large and their crashes are read through the linker map.

include(${CMAKE_CURRENT_LIST_DIR}/bp_cc.cmake)

set(GESHER_GRAPHICS_TARGETS)
macro(gesher_graphics_lib target)
  list(APPEND GESHER_GRAPHICS_TARGETS ${target})
  target_compile_options(${target} PRIVATE -g0)
  # Built by .github/workflows/graphics.yml (target gesher_graphics), or as
  # what dalvikvm links needs them; not by every `ninja`.
  set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL ON)
endmacro()

# --- FreeType (external/freetype: libft2) ----------------------------------
gesher_bp_cc(FT2 external/freetype libft2)
gesher_bp_library(ft2 FT2 QUIET)
target_link_libraries(ft2 PRIVATE png z)
gesher_graphics_lib(ft2)

# --- libpng (external/libpng) -----------------------------------------------
gesher_bp_cc(PNG external/libpng libpng)
gesher_bp_library(png PNG QUIET)
target_link_libraries(png PUBLIC z)
gesher_graphics_lib(png)

# --- libjpeg-turbo (external/libjpeg-turbo: libjpeg) ------------------------
# Without its SIMD code, which is NASM assembly: the portable C paths
# (jsimd_none.c) until a NASM step is worth it.
gesher_bp_cc(JPEG external/libjpeg-turbo libjpeg EXCLUDE "^simd/")
list(APPEND JPEG_SRCS "${AOSP_DIR}/external/libjpeg-turbo/jsimd_none.c")
list(REMOVE_ITEM JPEG_CFLAGS -DWITH_SIMD)
gesher_bp_library(jpeg JPEG QUIET)
gesher_graphics_lib(jpeg)

# --- libwebp (external/webp: libwebp-decode, libwebp-encode) ----------------
# The AVX2 and SSE4.1 halves are libraries of their own, as in Android.bp.
foreach(_m decode encode)
  foreach(_isa avx2 sse41)
    gesher_bp_cc(WEBP_${_m}_${_isa} external/webp libwebp-${_m}-${_isa})
    gesher_bp_library(webp_${_m}_${_isa} WEBP_${_m}_${_isa} QUIET)
    gesher_graphics_lib(webp_${_m}_${_isa})
  endforeach()
  gesher_bp_cc(WEBP_${_m} external/webp libwebp-${_m})
  gesher_bp_library(webp_${_m} WEBP_${_m} QUIET)
  target_include_directories(webp_${_m} PUBLIC ${AOSP_DIR}/external/webp/src)
  target_link_libraries(webp_${_m} PUBLIC webp_${_m}_avx2 webp_${_m}_sse41)
  gesher_graphics_lib(webp_${_m})
endforeach()
foreach(_t webp_decode_avx2 webp_decode_sse41 webp_encode_avx2 webp_encode_sse41)
  target_include_directories(${_t} PRIVATE ${AOSP_DIR}/external/webp/src)
endforeach()

# --- wuffs (external/wuffs-mirror-release-c) --------------------------------
gesher_bp_cc(WUFFS external/wuffs-mirror-release-c libwuffs_mirror_release_c)
gesher_bp_library(wuffs WUFFS QUIET)
gesher_graphics_lib(wuffs)

# --- Skia (external/skia: libskia) ------------------------------------------
# Left out of Android.bp's Windows variant:
# - Fontations, Skia's Rust font backend (libskia_rust_ffi, cxxbridge): FreeType
#   does the same work, and the C++ build needs no Rust toolchain.
# - PDF output (needs harfbuzz's subsetter): hwui's PdfDocument is device-only.
gesher_bp_cc(SKIA external/skia libskia
  EXCLUDE "fontations" "^src/pdf/")
gesher_bp_library(skia SKIA QUIET)
gesher_graphics_lib(skia)
# SkUserConfig.h: the Windows one gn_to_bp.py wrote, less what is left out.
set(_skcfg "${CMAKE_BINARY_DIR}/gen/skia/include/config/SkUserConfig.h")
file(READ "${AOSP_DIR}/external/skia/win/include/config/SkUserConfig.h" _cfg)
foreach(_d SK_FONTMGR_FONTATIONS_AVAILABLE SK_TYPEFACE_FACTORY_FONTATIONS SK_SUPPORT_PDF)
  string(REGEX REPLACE "#ifndef ${_d}\n#define ${_d}[^\n]*\n#endif" "// Gesher: no ${_d}" _cfg "${_cfg}")
endforeach()
file(CONFIGURE OUTPUT "${_skcfg}" CONTENT "${_cfg}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${AOSP_DIR}/external/skia/win/include/config/SkUserConfig.h")
target_include_directories(skia BEFORE PUBLIC "${CMAKE_BINARY_DIR}/gen/skia")
# Soong's Windows toolchain does not define NOGDI; Skia's Windows ports use GDI.
target_compile_options(skia PRIVATE -UNOGDI
  "SHELL:-include ${CMAKE_SOURCE_DIR}/compat/hwui/skia_prefix.h")
# Skia's headers have MSVC pragmas (#pragma warning) clang does not know.
target_compile_options(skia INTERFACE -Wno-unknown-pragmas)
target_link_libraries(skia PUBLIC ft2 png jpeg webp_decode webp_encode wuffs expat z cutils log
  # android.bp's host_ldlibs for Windows
  gdi32 oleaut32 ole32 opengl32 uuid windowscodecs usp10 dwrite)
target_include_directories(skia PUBLIC ${AOSP_DIR}/frameworks/native/libs/arect/include)

# A program that draws with Skia's CPU raster backend and checks the pixels
# (probes/graphics/skia_probe.cc), run in CI on Windows.
add_executable(skia_probe ${CMAKE_SOURCE_DIR}/probes/graphics/skia_probe.cc)
target_link_libraries(skia_probe PRIVATE skia)
target_compile_options(skia_probe PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
set_target_properties(skia_probe PROPERTIES EXCLUDE_FROM_ALL ON)

add_custom_target(gesher_graphics DEPENDS ${GESHER_GRAPHICS_TARGETS} skia_probe)
