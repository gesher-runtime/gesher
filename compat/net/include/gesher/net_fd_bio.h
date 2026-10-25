// Gesher: BoringSSL over gesher-base's socket fds (compat/net/fd_bio.cc).
//
// BoringSSL is a Windows library in Gesher, built without its socket BIO
// (OPENSSL_NO_SOCK, cmake/aosp/boringssl.cmake): SSL_set_fd would hand the fd
// to Winsock as a SOCKET, but a socket's fd is gesher-base's (ADR 0009).
#pragma once

#include <openssl/base.h>

// As SSL_set_fd: `ssl` reads and writes `fd` through gesher-base's recv and
// send. The fd is not closed when the BIO goes. Returns 1, or 0 on failure.
int gesher_SSL_set_fd(SSL* ssl, int fd);

// A BIO over gesher-base's socket fd `fd`, as BIO_new_socket(fd, close_flag).
BIO* gesher_BIO_new_fd_socket(int fd, int close_flag);
