// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Standalone Android reference for AHardwareBuffer socket sharing and ownership.
// Build with the NDK's <abi>-linux-android31-clang and -landroid.
#include <android/hardware_buffer.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) \
  do { \
    if (!(condition)) { \
      fprintf(stderr, "hardware buffer socket: line %d: %s\n", __LINE__, #condition); \
      return 1; \
    } \
  } while (0)

static const uint64_t kUsage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                               AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;

static int Sender(int socket) {
  const AHardwareBuffer_Desc desc = {64, 1, 1, AHARDWAREBUFFER_FORMAT_BLOB, kUsage, 0, 0, 0};
  AHardwareBuffer* buffer = NULL;
  CHECK(AHardwareBuffer_allocate(&desc, &buffer) == 0 && buffer != NULL);
  uint64_t id = 0;
  CHECK(AHardwareBuffer_getId(buffer, &id) == 0 && id != 0);
  void* data = NULL;
  CHECK(AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &data) == 0);
  ((uint8_t*)data)[0] = 83;
  CHECK(AHardwareBuffer_unlock(buffer, NULL) == 0);

  CHECK(send(socket, &id, sizeof id, 0) == sizeof id);
  CHECK(AHardwareBuffer_sendHandleToUnixSocket(buffer, socket) == 0);
  uint8_t ready = 0;
  CHECK(recv(socket, &ready, sizeof ready, 0) == sizeof ready && ready == 1);

  CHECK(AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &data) == 0);
  CHECK(((const uint8_t*)data)[0] == 109);
  CHECK(AHardwareBuffer_unlock(buffer, NULL) == 0);
  AHardwareBuffer_release(buffer);
  ready = 2;
  CHECK(send(socket, &ready, sizeof ready, 0) == sizeof ready);
  return 0;
}

static int Receiver(int socket) {
  uint64_t expected_id = 0;
  CHECK(recv(socket, &expected_id, sizeof expected_id, 0) == sizeof expected_id);
  AHardwareBuffer* imported = NULL;
  CHECK(AHardwareBuffer_recvHandleFromUnixSocket(socket, &imported) == 0 && imported != NULL);
  uint64_t id = 0;
  CHECK(AHardwareBuffer_getId(imported, &id) == 0 && id == expected_id);
  AHardwareBuffer_Desc desc = {0};
  AHardwareBuffer_describe(imported, &desc);
  CHECK(desc.width == 64 && desc.height == 1 && desc.layers == 1 &&
        desc.format == AHARDWAREBUFFER_FORMAT_BLOB && desc.usage == kUsage);

  void* data = NULL;
  CHECK(AHardwareBuffer_lock(imported, kUsage, -1, NULL, &data) == 0);
  CHECK(((const uint8_t*)data)[0] == 83);
  ((uint8_t*)data)[0] = 109;
  CHECK(AHardwareBuffer_unlock(imported, NULL) == 0);
  uint8_t ready = 1;
  CHECK(send(socket, &ready, sizeof ready, 0) == sizeof ready);
  CHECK(recv(socket, &ready, sizeof ready, 0) == sizeof ready && ready == 2);

  CHECK(AHardwareBuffer_lock(imported, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &data) == 0);
  CHECK(((const uint8_t*)data)[0] == 109);
  CHECK(AHardwareBuffer_unlock(imported, NULL) == 0);
  AHardwareBuffer_release(imported);
  return 0;
}

int main(void) {
  int sockets[2];
  CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) == 0);
  const pid_t child = fork();  // Fork before opening a buffer or using the mapper.
  CHECK(child >= 0);
  alarm(15);
  if (child == 0) {
    close(sockets[0]);
    const int result = Receiver(sockets[1]);
    close(sockets[1]);
    _exit(result);
  }
  close(sockets[1]);
  const int result = Sender(sockets[0]);
  close(sockets[0]);
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(result == 0);
  puts("hardware buffer socket: shared BLOB bytes, ID and independent lifetime passed");
  return 0;
}
