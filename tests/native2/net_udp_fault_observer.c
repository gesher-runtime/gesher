// CI-only actual Linux observation. No assumed EFAULT queue/copy semantics.
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void setup(int okay) { if (!okay) { perror("observer setup"); exit(2); } }
static void observe(int family, int api, int flags, int fault) {
  int rx = socket(family, SOCK_DGRAM, 0), tx = socket(family, SOCK_DGRAM, 0);
  setup(rx >= 0 && tx >= 0);
  struct sockaddr_storage address = {0}; address.ss_family = family;
  socklen_t length;
  if (family == AF_INET) {
    struct sockaddr_in* a = (void*)&address;
    a->sin_addr.s_addr = htonl(INADDR_LOOPBACK); length = sizeof(*a);
  } else {
    struct sockaddr_in6* a = (void*)&address;
    a->sin6_addr = in6addr_loopback; length = sizeof(*a);
  }
  setup(bind(rx, (void*)&address, length) == 0);
  setup(getsockname(rx, (void*)&address, &length) == 0);
  setup(sendto(tx, "abcdefghi", 9, 0, (void*)&address, length) == 9);
  long page = sysconf(_SC_PAGESIZE); setup(page > 0);
  unsigned char* memory = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  setup(memory != MAP_FAILED); memset(memory, 0xa5, (size_t)page);
  setup(mprotect(memory, (size_t)page, PROT_READ) == 0);
  void* bad = fault == 0 ? (void*)(uintptr_t)1 : fault == 1 ? NULL : memory;
  unsigned char prefix[4]; memset(prefix, 0xa5, sizeof(prefix));
  struct iovec iov[2] = {{bad, 4}, {NULL, 0}};
  if (fault == 3) { iov[0].iov_base = prefix; iov[1].iov_base = (void*)(uintptr_t)1; iov[1].iov_len = 4; }
  struct msghdr msg = {0}; msg.msg_iov = iov; msg.msg_iovlen = 2; msg.msg_flags = 0x5a5a;
  if (fault == 4) msg.msg_iov = (void*)(uintptr_t)1;
  struct msghdr* message = fault == 5 ? (void*)(uintptr_t)1 : &msg;
  struct sockaddr_storage peer = {0}; socklen_t peer_len = sizeof(peer);
  errno = 0;
  ssize_t n = api == 0 ? recv(rx, bad, 4, flags) :
              api == 1 ? recvfrom(rx, bad, 4, flags, (void*)&peer, &peer_len) :
                         recvmsg(rx, message, flags);
  int error = errno;
  unsigned char next[16] = {0}; errno = 0;
  ssize_t queued = recv(rx, next, sizeof(next), MSG_DONTWAIT);
  int queued_error = errno;
  setup(mprotect(memory, (size_t)page, PROT_READ | PROT_WRITE) == 0);
  printf("UDPFAULT family=%d api=%d flags=%d fault=%d n=%zd errno=%d out=%d prefix=%02x%02x%02x%02x readonly=%02x queue=%zd qerrno=%d qprefix=%02x%02x\n",
         family, api, flags, fault, n, error, msg.msg_flags,
         prefix[0], prefix[1], prefix[2], prefix[3], memory[0], queued, queued_error, next[0], next[1]);
  setup(munmap(memory, (size_t)page) == 0); close(rx); close(tx);
}
int main(void) {
  const int families[] = {AF_INET, AF_INET6};
  const int flags[] = {0, MSG_TRUNC, MSG_PEEK, MSG_PEEK | MSG_TRUNC};
  for (int f = 0; f < 2; ++f) for (int api = 0; api < 3; ++api)
    for (int k = 0; k < 4; ++k) for (int fault = 0; fault < (api == 2 ? 6 : 3); ++fault)
      observe(families[f], api, flags[k], fault);
  return 0;
}
