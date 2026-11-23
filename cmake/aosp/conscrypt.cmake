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

# com.gesher.net.WindowsNetworks's natives (the built-in library libgesher_net.so):
# Windows' networks for the connectivity service, and the INTERNET permission's
# switch (<gesher/inet.h>), plus the libgesher_nsd.so multicast query transport.
add_library(gesher_net_natives OBJECT ${_net}/windows_networks.cc ${_net}/windows_nsd.cc
  ${_net}/windows_nsd_resolve.cc ${_net}/windows_wifi.cc)
target_include_directories(gesher_net_natives PRIVATE ${_net}/include)
target_compile_options(gesher_net_natives PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(gesher_net_natives PUBLIC nativehelper iphlpapi ole32 oleaut32 ws2_32)
target_link_libraries(dalvikvm PRIVATE gesher_net_natives)

# Link the full NSD transport alongside the real guest socket/select exports.
# MinGW import members define callable thunks as well as __imp_* slots; direct
# Winsock imports with these names must not be selected by the transport.
# This is a link gate, not native socket/runtime qualification.
add_executable(nsd_windows_link_test
  ${CMAKE_SOURCE_DIR}/tests/net/nsd_windows_link_test.cc
  ${_net}/windows_nsd_resolve.cc)
target_include_directories(nsd_windows_link_test PRIVATE ${_net}/include)
target_compile_options(nsd_windows_link_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(nsd_windows_link_test PRIVATE gesher_posix nativehelper iphlpapi ws2_32)
