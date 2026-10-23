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

# --- liblzma (external/lzma, the LZMA SDK; Windows configuration) ----------
set(_lz "${AOSP_DIR}/external/lzma/C")
add_library(lzma STATIC
  ${_lz}/7zAlloc.c ${_lz}/7zArcIn.c ${_lz}/7zBuf2.c ${_lz}/7zBuf.c ${_lz}/7zCrc.c
  ${_lz}/7zCrcOpt.c ${_lz}/7zDec.c ${_lz}/7zFile.c ${_lz}/7zStream.c ${_lz}/Aes.c
  ${_lz}/AesOpt.c ${_lz}/Alloc.c ${_lz}/Bcj2.c ${_lz}/Bra86.c ${_lz}/Bra.c
  ${_lz}/BraIA64.c ${_lz}/CpuArch.c ${_lz}/Delta.c ${_lz}/LzFind.c ${_lz}/Lzma2Dec.c
  ${_lz}/Lzma2Enc.c ${_lz}/Lzma86Dec.c ${_lz}/Lzma86Enc.c ${_lz}/LzmaDec.c
  ${_lz}/LzmaEnc.c ${_lz}/LzmaLib.c ${_lz}/Ppmd7.c ${_lz}/Ppmd7Dec.c ${_lz}/Ppmd7Enc.c
  ${_lz}/Sha256.c ${_lz}/Sha256Opt.c ${_lz}/Sort.c ${_lz}/Xz.c ${_lz}/XzCrc64.c
  ${_lz}/XzCrc64Opt.c ${_lz}/XzDec.c ${_lz}/XzEnc.c ${_lz}/XzIn.c
  # target.windows
  ${_lz}/Bcj2Enc.c ${_lz}/DllSecur.c ${_lz}/LzFindMt.c ${_lz}/LzFindOpt.c
  ${_lz}/Lzma2DecMt.c ${_lz}/MtCoder.c ${_lz}/MtDec.c ${_lz}/Threads.c)
target_include_directories(lzma PUBLIC ${_lz})
target_compile_definitions(lzma PRIVATE Z7_ST)
target_compile_options(lzma PRIVATE -Wall -Wno-empty-body -Wno-enum-conversion
  -Wno-logical-op-parentheses -Wno-self-assign)

# --- liblz4 -----------------------------------------------------------------
set(_l4 "${AOSP_DIR}/external/lz4/lib")
add_library(lz4 STATIC ${_l4}/lz4.c ${_l4}/lz4hc.c ${_l4}/lz4frame.c ${_l4}/xxhash.c)
target_include_directories(lz4 PUBLIC ${_l4})

# --- cpu_features -----------------------------------------------------------
# Android.bp builds impl_x86_linux_or_android.c for every x86-64 target, which
# is empty on Windows; upstream's Windows implementation is used instead.
set(_cf "${AOSP_DIR}/external/cpu_features")
add_library(cpu_features STATIC
  ${_cf}/src/filesystem.c ${_cf}/src/stack_line_reader.c ${_cf}/src/string_view.c
  ${_cf}/src/impl_x86_windows.c)
target_include_directories(cpu_features PUBLIC ${_cf}/include)
target_compile_definitions(cpu_features PRIVATE STACK_LINE_READER_BUFFER_SIZE=1024)
target_compile_options(cpu_features PRIVATE -Wno-gnu-designator -Wno-unused-variable)

# --- libpng (external/libpng; host configuration, which leaves the SSE
# filters to Android's x86 targets) -----------------------------------------
set(_png "${AOSP_DIR}/external/libpng")
add_library(png STATIC
  ${_png}/png.c ${_png}/pngerror.c ${_png}/pngget.c ${_png}/pngmem.c ${_png}/pngpread.c
  ${_png}/pngread.c ${_png}/pngrio.c ${_png}/pngrtran.c ${_png}/pngrutil.c ${_png}/pngset.c
  ${_png}/pngtrans.c ${_png}/pngwio.c ${_png}/pngwrite.c ${_png}/pngwtran.c ${_png}/pngwutil.c
  # x86_64
  ${_png}/intel/filter_sse2_intrinsics.c ${_png}/intel/intel_init.c)
target_include_directories(png PUBLIC ${_png})
target_link_libraries(png PUBLIC z)
target_compile_options(png PRIVATE -std=gnu89 -Wall -Werror -Wno-unused-parameter)
