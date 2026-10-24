# libcore's native libraries and their dependencies, mirroring
# libcore/NativeCode.bp, libnativehelper/Android.bp and art/openjdkjvm.
#
# Android loads libjavacore.so, libopenjdk.so and libicu_jni.so with dlopen.
# Gesher links them into the executable instead and registers each one with
# gesher-base's dlopen as a built-in library (compat/posix/src/dlfcn.cc), so
# ART's LoadNativeLibrary finds them by name. Each library's JNI_OnLoad is
# renamed to JNI_OnLoad_<library>, the JNI convention for statically linked
# libraries, so they can coexist in one executable.

set(_lc "${AOSP_DIR}/libcore")
set(_nh "${AOSP_DIR}/libnativehelper")

# Soong file lists, read from the .bp files at configure time.
function(libcore_bp_files var module)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen/bp_files.py" ${module}
            "${_lc}/NativeCode.bp" "${_lc}/luni/src/main/native/Android.bp"
            "${_lc}/ojluni/src/main/native/Android.bp"
    OUTPUT_VARIABLE out COMMAND_ERROR_IS_FATAL ANY)
  string(STRIP "${out}" out)
  string(REPLACE "\n" ";" out "${out}")
  set(${var} ${out} PARENT_SCOPE)
endfunction()

# --- libexpat ---------------------------------------------------------------
set(_ex "${AOSP_DIR}/external/expat")
add_library(expat STATIC
  ${_ex}/expat/lib/xmlparse.c ${_ex}/expat/lib/xmlrole.c ${_ex}/expat/lib/xmltok.c)
target_include_directories(expat PUBLIC ${_ex}/expat/lib PRIVATE ${_ex})
target_compile_definitions(expat PRIVATE HAVE_EXPAT_CONFIG_H)
target_compile_options(expat PRIVATE -fno-strict-aliasing -UWIN32_LEAN_AND_MEAN
  "SHELL:-include stdio.h")

# --- libfdlibm --------------------------------------------------------------
set(_fd "${AOSP_DIR}/external/fdlibm")
file(GLOB _fdlibm_srcs "${_fd}/[eks]_*.c" "${_fd}/w_*.c")
list(SORT _fdlibm_srcs)
add_library(fdlibm STATIC ${_fdlibm_srcs})
target_include_directories(fdlibm PUBLIC ${_fd})
target_compile_definitions(fdlibm PRIVATE _IEEE_LIBM __LITTLE_ENDIAN)
target_compile_options(fdlibm PRIVATE -fno-strict-aliasing -Wno-sign-compare
  -Wno-dangling-else -Wno-unknown-pragmas -Wno-logical-op-parentheses
  -Wno-sometimes-uninitialized -Wno-implicit-function-declaration
  -Wno-deprecated-non-prototype -Wno-implicit-int)

# --- libnativehelper (libnativehelper_any_vm + libnativehelper) -------------
add_library(nativehelper STATIC
  ${_nh}/DlHelp.c ${_nh}/ExpandableString.c ${_nh}/JNIHelp.c
  ${_nh}/JNIPlatformHelp.c ${_nh}/JniConstants.c ${_nh}/file_descriptor_jni.c)
target_include_directories(nativehelper PUBLIC
  ${_nh}/header_only_include ${_nh}/include ${_nh}/include_jni
  ${_nh}/include_platform ${_nh}/include_platform_header_only)
target_link_libraries(nativehelper PUBLIC log)

# --- libopenjdkjvm (art/openjdkjvm): the JVM_* interface over ART -----------
add_library(art_openjdkjvm OBJECT ${_art}/openjdkjvm/OpenjdkJvm.cc)
target_link_libraries(art_openjdkjvm PUBLIC art_runtime_headers)
add_dependencies(art_openjdkjvm art_asm_support_headers)

# --- libjavacore, libandroidio ----------------------------------------------
libcore_bp_files(_javacore_srcs luni_native_srcs)
libcore_bp_files(_androidio_srcs libandroidio_srcs)
# Not ported yet; their natives are registered as missing (see
# compat/libcore/unported.cc), so calling one throws UnsatisfiedLinkError.
# BoringSSL (NativeBN) is not built yet.
set(GESHER_LIBCORE_UNPORTED
  luni/src/main/native/libcore_math_NativeBN.cpp
  CACHE STRING "libcore native sources not built yet")
foreach(f ${GESHER_LIBCORE_UNPORTED})
  list(REMOVE_ITEM _javacore_srcs "${_lc}/${f}")
endforeach()

set(_libcore_flags -Wall -Wextra -Wno-unused-parameter
  $<$<COMPILE_LANGUAGE:CXX>:-DU_USING_ICU_NAMESPACE=0>)

add_library(libcore_javacore OBJECT ${_javacore_srcs} ${_androidio_srcs})
target_include_directories(libcore_javacore PRIVATE ${_lc}/luni/src/main/native)
# ICU through the NDK's libicu, as on Android (cmake/aosp/icu.cmake).
target_link_libraries(libcore_javacore PUBLIC nativehelper base log expat z ziparchive
  icu_ndk_headers)
target_compile_options(libcore_javacore PRIVATE ${_libcore_flags})
set_source_files_properties("${_lc}/luni/src/main/native/Register.cpp" PROPERTIES
  COMPILE_DEFINITIONS "JNI_OnLoad=JNI_OnLoad_javacore;JNI_OnUnload=JNI_OnUnload_javacore")

# --- libopenjdk -------------------------------------------------------------
libcore_bp_files(_openjdk_srcs libopenjdk_native_srcs)
# UNIXProcess starts processes with vfork. (epoll and inotify, for
# LinuxWatchService, are gesher-base's, over its file descriptors.)
set(GESHER_OPENJDK_UNPORTED
  ojluni/src/main/native/UNIXProcess_md.c
  CACHE STRING "libopenjdk native sources not built yet")
foreach(f ${GESHER_OPENJDK_UNPORTED})
  list(REMOVE_ITEM _openjdk_srcs "${_lc}/${f}")
endforeach()
add_library(libcore_openjdk OBJECT ${_openjdk_srcs})
target_include_directories(libcore_openjdk PRIVATE ${_lc}/luni/src/main/native
  ${_lc}/ojluni/src/main/native)
target_link_libraries(libcore_openjdk PUBLIC nativehelper base log fdlibm z icu_ndk_headers)
# OpenJDK's native code chooses its Linux paths with __linux__ (and its
# Windows ones with WIN32), and gesher-base provides Linux's interfaces. Only
# for C: libc++'s headers test __linux__ too, and must not see it.
target_compile_definitions(libcore_openjdk PRIVATE _LARGEFILE64_SOURCE _GNU_SOURCE LINUX
  $<$<COMPILE_LANGUAGE:C>:__linux__>)
target_compile_options(libcore_openjdk PRIVATE $<$<COMPILE_LANGUAGE:C>:-UWIN32>)
# libjavacore and libopenjdk each have a class JniConstants, apart in their
# .so files on Android; linked into one executable they would clash.
target_compile_definitions(libcore_openjdk PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:JniConstants=OpenjdkJniConstants>)
target_compile_options(libcore_openjdk PRIVATE ${_libcore_flags} -Wno-unused-variable
  -Wno-parentheses-equality -Wno-constant-logical-operand -Wno-sometimes-uninitialized)
set_source_files_properties("${_lc}/ojluni/src/main/native/OnLoad.cpp" PROPERTIES
  COMPILE_DEFINITIONS "JNI_OnLoad=JNI_OnLoad_openjdk")

# --- linked into dalvikvm -----------------------------------------------------
# compat/libcore/builtins.cc registers the libraries with gesher-base's
# dlopen; unported.cc stands in for the registration functions of the
# sources listed above as not ported.
add_library(libcore_builtins OBJECT
  ${CMAKE_SOURCE_DIR}/compat/libcore/builtins.cc ${CMAKE_SOURCE_DIR}/compat/libcore/unported.cc)
target_link_libraries(libcore_builtins PRIVATE nativehelper)
foreach(f ${GESHER_LIBCORE_UNPORTED} ${GESHER_OPENJDK_UNPORTED})
  get_filename_component(_name "${f}" NAME_WE)
  target_compile_definitions(libcore_builtins PRIVATE GESHER_UNPORTED_${_name})
endforeach()
target_link_libraries(dalvikvm PRIVATE libcore_builtins libcore_javacore libcore_openjdk
  art_openjdkjvm icu_jni)
