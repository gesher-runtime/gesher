# external/* libraries, mirroring their Android.bp Windows configuration.

# --- zlib (external/zlib, Chromium's zlib) ---------------------------------
set(_z "${AOSP_DIR}/external/zlib")
add_library(z STATIC
  ${_z}/adler32.c ${_z}/adler32_simd.c ${_z}/compress.c ${_z}/cpu_features.c
  ${_z}/crc32.c ${_z}/crc32_simd.c ${_z}/crc_folding.c ${_z}/deflate.c
  ${_z}/gzclose.c ${_z}/gzlib.c ${_z}/gzread.c ${_z}/gzwrite.c ${_z}/infback.c
  ${_z}/inffast.c ${_z}/inflate.c ${_z}/inftrees.c ${_z}/trees.c
  ${_z}/uncompr.c ${_z}/zutil.c)
target_include_directories(z PUBLIC ${_z})
target_compile_definitions(z PRIVATE
  HAVE_HIDDEN ZLIB_CONST CHROMIUM_ZLIB_NO_CASTAGNOLI
  ADLER32_SIMD_SSSE3 DEFLATE_CHUNK_WRITE_64LE INFLATE_CHUNK_READ_64LE X86_WINDOWS)
target_compile_options(z PRIVATE -O3 -Wno-deprecated-non-prototype -Wno-unused -Wno-unused-parameter)

# --- tinyxml2 ---------------------------------------------------------------
set(_tx "${AOSP_DIR}/external/tinyxml2")
add_library(tinyxml2 STATIC ${_tx}/tinyxml2.cpp)
target_include_directories(tinyxml2 PUBLIC ${_tx})
target_compile_options(tinyxml2 PRIVATE -Wno-implicit-fallthrough)

# --- fmtlib -----------------------------------------------------------------
set(_fmt "${AOSP_DIR}/external/fmtlib")
add_library(fmtlib STATIC ${_fmt}/src/format.cc)
target_include_directories(fmtlib PUBLIC ${_fmt}/include)
target_compile_options(fmtlib PRIVATE -UNDEBUG)
# format.h uses malloc/free without <cstdlib>; Android's libc++ includes it
# transitively, llvm-mingw's does not.
target_compile_options(fmtlib PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdlib>")
