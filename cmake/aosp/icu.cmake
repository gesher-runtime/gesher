# external/icu: ICU4C, the NDK's libicu over it, and libicu_jni, mirroring
# their Android.bp host configuration.
#
# ICU4C is built for its MinGW platform (Win32 for its own platform code) and
# loads its data as on Android and the ART host: libandroidicuinit maps
# $ANDROID_I18N_ROOT/etc/icu/icudt<N>l.dat (the prebuilt
# icu4c/source/stubdata/icudt<N>l.dat) through gesher-base's open and mmap,
# on ICU's first data access (icu patch 0001 enables it on Windows).

set(_icu "${AOSP_DIR}/external/icu")
set(_icu4c "${_icu}/icu4c/source")

# Everything is linked statically: without U_STATIC_IMPLEMENTATION, ICU's
# headers declare the API dllimport on Windows.
add_library(icu_headers INTERFACE)
target_include_directories(icu_headers INTERFACE
  ${_icu}/android_icu4c/include ${_icu4c}/common ${_icu4c}/i18n)
target_compile_definitions(icu_headers INTERFACE U_STATIC_IMPLEMENTATION
  UCONFIG_USE_ML_PHRASE_BREAKING=1)

set(_icu4c_flags -Wall -Wno-ambiguous-reversed-operator -Wno-deprecated-declarations
  -Wno-unused-parameter -Wno-unused-const-variable -Wno-unneeded-internal-declaration
  -Wno-missing-field-initializers -Wno-sign-compare)

# --- libandroidicuinit --------------------------------------------------------
set(_aii "${_icu}/libandroidicuinit")
add_library(androidicuinit STATIC ${_aii}/IcuRegistration.cpp ${_aii}/android_icu_init.cpp)
target_include_directories(androidicuinit PUBLIC ${_aii}/include PRIVATE ${_aii})
target_link_libraries(androidicuinit PRIVATE icu_headers)
target_compile_options(androidicuinit PRIVATE -Wall)

# --- libicuuc (with libicuuc_stubdata, the empty linked-in data) --------------
file(GLOB _icuuc_srcs "${_icu4c}/common/*.cpp")
list(SORT _icuuc_srcs)
add_library(icuuc STATIC ${_icuuc_srcs} ${_icu4c}/stubdata/stubdata.cpp)
target_link_libraries(icuuc PUBLIC icu_headers PRIVATE androidicuinit)
target_compile_definitions(icuuc PRIVATE _REENTRANT U_COMMON_IMPLEMENTATION)
target_compile_options(icuuc PRIVATE ${_icu4c_flags})

# --- libicui18n ---------------------------------------------------------------
file(GLOB _icui18n_srcs "${_icu4c}/i18n/*.cpp")
list(SORT _icui18n_srcs)
add_library(icui18n STATIC ${_icui18n_srcs})
target_link_libraries(icui18n PUBLIC icu_headers icuuc)
target_compile_definitions(icui18n PRIVATE _REENTRANT U_I18N_IMPLEMENTATION)
target_compile_options(icui18n PRIVATE ${_icu4c_flags} -Wno-unreachable-code-loop-increment)

# --- libicu: the NDK's stable C API (unversioned names) over ICU4C ------------
# Users compile against ndk_headers (icu_ndk_headers), which must not meet
# ICU4C's own headers: shim.cpp forwards each name to ICU4C's versioned one.
add_library(icu_ndk STATIC ${_icu}/libicu/src/shim.cpp)
# PRIVATE: ICU4C is still linked in, but its headers stay with the shim.
target_link_libraries(icu_ndk PRIVATE icu_headers icui18n icuuc)
target_compile_definitions(icu_ndk PRIVATE U_SHOW_CPLUSPLUS_API=0 U_SHOW_CPLUSPLUS_HEADER_API=0)
target_compile_options(icu_ndk PRIVATE ${_icu4c_flags})

add_library(icu_ndk_headers INTERFACE)
target_include_directories(icu_ndk_headers INTERFACE ${_icu}/libicu/ndk_headers)
# uconfig.h reads uconfig_local.h (no renaming, the NDK's API only) when the
# platform build defines ANDROID; Gesher does not define ANDROID, which would
# switch on Android-only code elsewhere, so it asks for the file directly.
target_compile_definitions(icu_ndk_headers INTERFACE U_STATIC_IMPLEMENTATION UCONFIG_USE_LOCAL)
target_link_libraries(icu_ndk_headers INTERFACE icu_ndk log)  # uconfig_local.h: <android/log.h>

# --- libcutils' tracing, as on the host (trace-host.cpp: no-ops) --------------
add_library(cutils_trace STATIC ${_core}/libcutils/trace-host.cpp)
target_include_directories(cutils_trace PUBLIC ${_core}/libcutils/include)

# --- libicu_jni (android_icu4j/libcore_bridge/src/native) ---------------------
file(GLOB _icu_jni_srcs "${_icu}/android_icu4j/libcore_bridge/src/native/*.cpp")
list(SORT _icu_jni_srcs)
add_library(icu_jni OBJECT ${_icu_jni_srcs})
target_link_libraries(icu_jni PUBLIC icui18n icuuc nativehelper base log cutils_trace)
target_compile_definitions(icu_jni PRIVATE U_USING_ICU_NAMESPACE=0
  # libjavacore has a JniConstants and a maybeThrowIcuException too, apart in
  # their .so files on Android; linked into one executable they would clash.
  JniConstants=IcuJniConstants maybeThrowIcuException=IcuJniMaybeThrowIcuException)
target_compile_options(icu_jni PRIVATE -Wall -Wextra)
set_source_files_properties("${_icu}/android_icu4j/libcore_bridge/src/native/Register.cpp"
  PROPERTIES COMPILE_DEFINITIONS "JNI_OnLoad=JNI_OnLoad_icu_jni;JNI_OnUnload=JNI_OnUnload_icu_jni")
