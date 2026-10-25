// Gesher: a BoringSSL BIO over gesher-base's socket fds (<gesher/net_fd_bio.h>).
//
// Compiled on gesher-base's side (Linux's <sys/socket.h>, errno numbering):
// recv, send and close here are gesher-base's, which find the Winsock socket
// behind the fd (ADR 0009). It replaces BoringSSL's socket BIO, which Gesher
// leaves out (OPENSSL_NO_SOCK), for Conscrypt's file descriptor sockets.

#include <gesher/net_fd_bio.h>

#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/ssl.h>

namespace {

int Fd(BIO* bio) { return static_cast<int>(reinterpret_cast<intptr_t>(BIO_get_data(bio))); }

bool ShouldRetry(int error) {
  return error == EAGAIN || error == EINTR || error == EINPROGRESS || error == EALREADY;
}

int FdRead(BIO* bio, char* out, int len) {
  BIO_clear_retry_flags(bio);
  if (out == nullptr || len <= 0) return 0;
  ssize_t n = recv(Fd(bio), out, static_cast<size_t>(len), 0);
  if (n < 0 && ShouldRetry(errno)) BIO_set_retry_read(bio);
  return static_cast<int>(n);
}

int FdWrite(BIO* bio, const char* in, int len) {
  BIO_clear_retry_flags(bio);
  if (len <= 0) return 0;
  ssize_t n = send(Fd(bio), in, static_cast<size_t>(len), 0);
  if (n < 0 && ShouldRetry(errno)) BIO_set_retry_write(bio);
  return static_cast<int>(n);
}

int FdPuts(BIO* bio, const char* in) {
  size_t len = 0;
  while (in[len] != '\0') len++;
  return FdWrite(bio, in, static_cast<int>(len));
}

long FdCtrl(BIO* bio, int cmd, long num, void* ptr) {
  switch (cmd) {
    case BIO_C_SET_FD:
      BIO_set_data(bio, reinterpret_cast<void*>(static_cast<intptr_t>(*static_cast<int*>(ptr))));
      BIO_set_shutdown(bio, static_cast<int>(num));
      BIO_set_init(bio, 1);
      return 1;
    case BIO_C_GET_FD:
      if (!BIO_get_init(bio)) return -1;
      if (ptr != nullptr) *static_cast<int*>(ptr) = Fd(bio);
      return Fd(bio);
    case BIO_CTRL_GET_CLOSE:
      return BIO_get_shutdown(bio);
    case BIO_CTRL_SET_CLOSE:
      BIO_set_shutdown(bio, static_cast<int>(num));
      return 1;
    case BIO_CTRL_FLUSH:
      return 1;
    default:
      return 0;
  }
}

int FdCreate(BIO* bio) {
  BIO_set_data(bio, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
  return 1;
}

int FdDestroy(BIO* bio) {
  if (BIO_get_shutdown(bio) && BIO_get_init(bio)) close(Fd(bio));
  BIO_set_init(bio, 0);
  return 1;
}

const BIO_METHOD* FdMethod() {
  static const BIO_METHOD* method = [] {
    BIO_METHOD* m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK | BIO_TYPE_DESCRIPTOR,
                                 "gesher socket fd");
    BIO_meth_set_write(m, FdWrite);
    BIO_meth_set_read(m, FdRead);
    BIO_meth_set_puts(m, FdPuts);
    BIO_meth_set_ctrl(m, FdCtrl);
    BIO_meth_set_create(m, FdCreate);
    BIO_meth_set_destroy(m, FdDestroy);
    return m;
  }();
  return method;
}

}  // namespace

BIO* gesher_BIO_new_fd_socket(int fd, int close_flag) {
  BIO* bio = BIO_new(FdMethod());
  if (bio == nullptr) return nullptr;
  BIO_ctrl(bio, BIO_C_SET_FD, close_flag, &fd);
  return bio;
}

int gesher_SSL_set_fd(SSL* ssl, int fd) {
  BIO* bio = gesher_BIO_new_fd_socket(fd, BIO_NOCLOSE);
  if (bio == nullptr) return 0;
  SSL_set_bio(ssl, bio, bio);  // one reference, for both directions
  return 1;
}
