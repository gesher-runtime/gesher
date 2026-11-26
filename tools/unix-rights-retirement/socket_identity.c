// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// AF_UNIX identity and real byte delivery after a descriptor transfer.
#define _GNU_SOURCE 1
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int checks, failures;

static int Check(int condition, const char* description) {
  ++checks;
  if (!condition) {
    ++failures;
    fprintf(stderr, "FAIL: %s\n", description);
  }
  return condition;
}

static int OptionEquals(int fd, int option, int expected) {
  int value = -1;
  socklen_t size = sizeof(value);
  return getsockopt(fd, SOL_SOCKET, option, &value, &size) == 0 &&
         size == sizeof(value) && value == expected;
}

static int UnnamedAddress(int fd, int peer) {
  struct sockaddr_un address;
  memset(&address, 0x5a, sizeof(address));
  socklen_t size = sizeof(address);
  int status = peer ? getpeername(fd, (struct sockaddr*)&address, &size)
                    : getsockname(fd, (struct sockaddr*)&address, &size);
  return status == 0 && size == offsetof(struct sockaddr_un, sun_path) &&
         address.sun_family == AF_UNIX && address.sun_path[0] == 0x5a;
}

static void CheckIdentity(int fd, int type) {
  Check(OptionEquals(fd, SO_TYPE, type), "logical Unix socket type");
  Check(OptionEquals(fd, SO_DOMAIN, AF_UNIX), "logical Unix socket domain");
  Check(OptionEquals(fd, SO_PROTOCOL, 0), "Unix protocol");
  Check(UnnamedAddress(fd, 0), "unnamed local address");
  Check(UnnamedAddress(fd, 1), "unnamed peer address");
}

static int TransferSocket(int fd, const int transport[2]) {
  union {
    struct cmsghdr alignment;
    unsigned char bytes[CMSG_SPACE(sizeof(int))];
  } control = {0};
  char data = 's';
  struct iovec io = {&data, sizeof(data)};
  struct msghdr message = {
      .msg_iov = &io, .msg_iovlen = 1,
      .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes),
  };
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(sizeof(fd));
  memcpy(CMSG_DATA(header), &fd, sizeof(fd));
  if (!Check(sendmsg(transport[0], &message, 0) == 1, "send a live socket through SCM_RIGHTS")) return -1;
  data = 0;
  memset(control.bytes, 0, sizeof(control.bytes));
  if (!Check(recvmsg(transport[1], &message, MSG_CMSG_CLOEXEC) == 1 && data == 's',
             "receive the SCM_RIGHTS payload")) return -1;
  header = CMSG_FIRSTHDR(&message);
  if (!Check(header != NULL && header->cmsg_level == SOL_SOCKET &&
                 header->cmsg_type == SCM_RIGHTS && header->cmsg_len == CMSG_LEN(sizeof(fd)),
             "receive one socket descriptor")) return -1;
  int imported = -1;
  memcpy(&imported, CMSG_DATA(header), sizeof(imported));
  return imported;
}

static void Probe(int type, const int transport[2]) {
  int pair[2];
  if (!Check(socketpair(AF_UNIX, type, 0, pair) == 0, "open a Unix socketpair")) return;
  CheckIdentity(pair[0], type);
  int alias = dup(pair[0]);
  if (Check(alias >= 0, "duplicate the endpoint")) {
    close(pair[0]);
    CheckIdentity(alias, type);
    int imported = TransferSocket(alias, transport);
    close(alias);
    if (Check(imported >= 0, "adopt the socket endpoint")) {
      CheckIdentity(imported, type);
      Check(fcntl(imported, F_GETFD) == FD_CLOEXEC, "received endpoint is close-on-exec");
      struct sockaddr_un source;
      memset(&source, 0x5a, sizeof(source));
      socklen_t source_size = sizeof(source);
      char data = 0;
      Check(send(pair[1], "a", 1, 0) == 1 &&
                recvfrom(imported, &data, 1, 0, (struct sockaddr*)&source,
                         &source_size) == 1 &&
                data == 'a' && source_size == 0 &&
                ((unsigned char*)&source)[0] == 0x5a,
            "recvfrom leaves an unnamed sender address untouched");
      struct iovec io = {&data, sizeof(data)};
      struct msghdr message = {
          .msg_name = &source, .msg_namelen = sizeof(source),
          .msg_iov = &io, .msg_iovlen = 1,
      };
      Check(send(pair[1], "b", 1, 0) == 1 &&
                recvmsg(imported, &message, 0) == 1 && data == 'b' &&
                message.msg_namelen == 0 && ((unsigned char*)&source)[0] == 0x5a,
            "recvmsg leaves an unnamed sender address untouched");
      close(imported);
    }
  } else {
    close(pair[0]);
  }
  close(pair[1]);
}

int main(void) {
  int transport[2];
  if (Check(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, transport) == 0, "open the descriptor carrier")) {
    Probe(SOCK_STREAM, transport);
    Probe(SOCK_DGRAM, transport);
    Probe(SOCK_SEQPACKET, transport);
    close(transport[0]);
    close(transport[1]);
  }
  printf("unix pair identity: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
