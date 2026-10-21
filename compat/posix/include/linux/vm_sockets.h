// gesher-base: <linux/vm_sockets.h>. VM sockets do not exist on Windows.
#pragma once

#include <sys/socket.h>

#define VMADDR_CID_ANY  0xFFFFFFFFu
#define VMADDR_PORT_ANY 0xFFFFFFFFu
#define VMADDR_CID_LOCAL 1
#define VMADDR_CID_HOST 2

struct sockaddr_vm {
  sa_family_t svm_family;
  unsigned short svm_reserved1;
  unsigned int svm_port;
  unsigned int svm_cid;
  unsigned char svm_flags;
  unsigned char svm_zero[3];
};
