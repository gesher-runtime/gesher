# BoringSSL (external/boringssl) for Windows: libcrypto and libssl, which
# libcore's NativeBN and Conscrypt's libjavacrypto use (docs/network.md,
# ADR 0037). The source lists are the ones the project generates for CMake
# (android-sources.cmake, the same files sources.bp gives Soong).
#
# BoringSSL is built as a Windows library (OPENSSL_WINDOWS: Win32 threads and
# locks, ProcessPrng for randomness), as ICU4C is.
# No assembly yet: the x86-64 Windows assembly is NASM, which the build host
# does not have (OPENSSL_NO_ASM: the C implementations, AES and GHASH included).
# OPENSSL_NO_SOCK leaves out the socket and connect BIOs and SSL_set_fd: on
# Windows they call Winsock's send, recv and connect, whose import library
# members also define the names gesher-base defines for its own fds
# (ADR 0009); Conscrypt's one caller uses compat/net/fd_bio.cc's.

set(_bssl "${AOSP_DIR}/external/boringssl")
set(BORINGSSL_ROOT "${_bssl}/")
include("${_bssl}/android-sources.cmake")

# What Soong's boringssl_flags_common gives every module, less the shared
# library's visibility flags (the libraries are linked into the executables).
set(_bssl_defs OPENSSL_SMALL OPENSSL_NO_ASM OPENSSL_NO_SOCK)
set(_bssl_flags -Wno-unused-parameter -Wno-unknown-pragmas -Wno-unused-command-line-argument
  -fno-strict-aliasing -Wno-deprecated-declarations)

add_library(boringssl_headers INTERFACE)
target_include_directories(boringssl_headers INTERFACE ${_bssl}/src/include)
target_compile_definitions(boringssl_headers INTERFACE ${_bssl_defs})

add_library(boringssl_crypto STATIC ${crypto_sources})
target_link_libraries(boringssl_crypto PUBLIC boringssl_headers ws2_32)
target_compile_definitions(boringssl_crypto PRIVATE BORINGSSL_IMPLEMENTATION)
target_compile_options(boringssl_crypto PRIVATE ${_bssl_flags})

add_library(boringssl_ssl STATIC ${ssl_sources})
target_link_libraries(boringssl_ssl PUBLIC boringssl_crypto)
target_compile_definitions(boringssl_ssl PRIVATE BORINGSSL_IMPLEMENTATION)
target_compile_options(boringssl_ssl PRIVATE ${_bssl_flags})
