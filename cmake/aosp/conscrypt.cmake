# Conscrypt's native library, libjavacrypto (external/conscrypt's
# common/src/jni, as Android.bp's libjavacrypto builds it), over BoringSSL
# (cmake/aosp/boringssl.cmake), and Gesher's networking pieces in compat/net
# (docs/network.md, ADR 0037), linked into dalvikvm.
#
# compat/net/javacrypto.cc registers it as the built-in library
# libjavacrypto.so, which Conscrypt's NativeCryptoJni loads; its JNI_OnLoad
# first brings the system CA certificates up to date from Windows' store
# (compat/net/trust_store.cc). Conscrypt's sources are patched to take the
# POSIX side of their #ifdefs (gesher-base) rather than their own Windows
# ports, and to register their natives through System V thunks
# (patches/external/conscrypt).

set(_cs "${AOSP_DIR}/external/conscrypt")
set(_net "${CMAKE_SOURCE_DIR}/compat/net")
file(GLOB _cs_srcs "${_cs}/common/src/jni/main/cpp/conscrypt/*.cc")
list(SORT _cs_srcs)

add_library(conscrypt_jni OBJECT ${_cs_srcs}
  ${_net}/javacrypto.cc ${_net}/trust_store.cc ${_net}/fd_bio.cc)
target_include_directories(conscrypt_jni PRIVATE ${_cs}/common/src/jni/main/include
  ${_net}/include ${_net})
# STATIC_LIB: JNI_OnLoad_conscrypt instead of JNI_OnLoad; JNI_JARJAR_PREFIX:
# the platform's com.android.org.conscrypt classes, as Android.bp sets it.
target_compile_definitions(conscrypt_jni PRIVATE STATIC_LIB JNI_JARJAR_PREFIX=com/android/)
target_compile_options(conscrypt_jni PRIVATE -Wall -Wextra -Wno-unused-parameter
  -Wno-missing-field-initializers)
target_link_libraries(conscrypt_jni PUBLIC nativehelper log boringssl_ssl crypt32)

target_link_libraries(dalvikvm PRIVATE conscrypt_jni)
