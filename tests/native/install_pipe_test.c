/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
/* Exercise the production private factory directly; this executable is not shipped. */
#include "../../compat/install/install_pipe.c"
#include <stdio.h>
#include <string.h>

static int checks;
#define CHECK(value) do { if (!(value)) { \
  fprintf(stderr, "install_pipe_test failed at line %d, winerr=%lu\n", __LINE__, GetLastError()); \
  ExitProcess(1); } ++checks; } while (0)

static HANDLE Handle(int fd) { return (HANDLE)_get_osfhandle(fd); }

static void AccessControl(HANDLE pipe) {
  PACL actual = NULL;
  PSECURITY_DESCRIPTOR security = NULL;
  CHECK(GetSecurityInfo(pipe, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                        NULL, NULL, &actual, NULL, &security) == ERROR_SUCCESS);
  CHECK(actual != NULL && actual->AceCount == 1);
  SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0;
  CHECK(GetSecurityDescriptorControl(security, &control, &revision)
      && (control & SE_DACL_PROTECTED) != 0);
  ACCESS_ALLOWED_ACE* ace = NULL;
  CHECK(GetAce(actual, 0, (void**)&ace) && ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE
      && ace->Header.AceFlags == 0);
  // Independently query the process token rather than reusing the factory's ACL builder.
  HANDLE token = NULL; DWORD size = 0;
  CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
  CHECK(!GetTokenInformation(token, TokenUser, NULL, 0, &size)
      && GetLastError() == ERROR_INSUFFICIENT_BUFFER && size > 0 && size <= 65536);
  TOKEN_USER* user = (TOKEN_USER*)HeapAlloc(GetProcessHeap(), 0, size);
  CHECK(user != NULL && GetTokenInformation(token, TokenUser, user, size, &size)
      && EqualSid(&ace->SidStart, user->User.Sid));
  CHECK((ace->Mask & FILE_GENERIC_READ) == FILE_GENERIC_READ
      && (ace->Mask & FILE_GENERIC_WRITE) == FILE_GENERIC_WRITE);
  HeapFree(GetProcessHeap(), 0, user); CloseHandle(token); LocalFree(security);
}

static void RoundTrip(void) {
  int fds[2]; CHECK(CreatePair(fds));
  CHECK(fds[0] >= 0 && fds[1] >= 0 && fds[0] != fds[1]);
  CHECK(GetFileType(Handle(fds[0])) == FILE_TYPE_PIPE
      && GetFileType(Handle(fds[1])) == FILE_TYPE_PIPE);
  DWORD flags = 0, instances = 0, inherit = HANDLE_FLAG_INHERIT;
  CHECK(GetNamedPipeInfo(Handle(fds[0]), &flags, NULL, NULL, &instances)
      && (flags & PIPE_SERVER_END) != 0 && (flags & PIPE_TYPE_MESSAGE) == 0 && instances == 1);
  CHECK(GetHandleInformation(Handle(fds[0]), &inherit) && (inherit & HANDLE_FLAG_INHERIT) == 0);
  CHECK(GetHandleInformation(Handle(fds[1]), &inherit) && (inherit & HANDLE_FLAG_INHERIT) == 0);
  AccessControl(Handle(fds[0]));
  BYTE input[8192], output[8192];
  for (SIZE_T i = 0; i < sizeof(input); ++i) input[i] = (BYTE)(i ^ 0xa5);
  CHECK(_write(fds[1], input, sizeof(input)) == sizeof(input));
  CHECK(_read(fds[0], output, sizeof(output)) == sizeof(output)
      && memcmp(input, output, sizeof(input)) == 0);
  const BYTE ack[8] = {0, 0, 0, 2, 0, 0, 0, 0};
  CHECK(_write(fds[0], ack, sizeof(ack)) == sizeof(ack));
  CHECK(_read(fds[1], output, sizeof(ack)) == sizeof(ack)
      && memcmp(ack, output, sizeof(ack)) == 0);
  // The same ordinary HANDLE duplication and CRT adoption used by Binder.
  HANDLE duplicate = NULL;
  CHECK(DuplicateHandle(GetCurrentProcess(), Handle(fds[1]), GetCurrentProcess(),
      &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
  int copied = _open_osfhandle((intptr_t)duplicate, _O_RDWR | _O_BINARY);
  CHECK(copied >= 0 && _close(fds[1]) == 0);
  CHECK(_write(copied, ack, sizeof(ack)) == sizeof(ack)
      && _read(fds[0], output, sizeof(ack)) == sizeof(ack)
      && memcmp(ack, output, sizeof(ack)) == 0);
  CHECK(_close(copied) == 0 && _close(fds[0]) == 0);
}

struct ReadState { int fd; int result; HANDLE entered; };
static DWORD WINAPI ReadBlocked(void* pointer) {
  struct ReadState* state = (struct ReadState*)pointer;
  BYTE byte;
  SetEvent(state->entered);
  state->result = _read(state->fd, &byte, 1);
  return 0;
}

static void Cancellation(void) {
  int fds[2]; CHECK(CreatePair(fds));
  HANDLE raw = Handle(fds[0]);
  struct ReadState state = {fds[0], 99, CreateEventW(NULL, TRUE, FALSE, NULL)};
  CHECK(state.entered != NULL);
  HANDLE reader = CreateThread(NULL, 0, ReadBlocked, &state, 0, NULL);
  CHECK(reader != NULL && WaitForSingleObject(state.entered, 1000) == WAIT_OBJECT_0);
  // Repeat cancellation, as the Java owner must: cancellation may precede the read.
  DWORD wait = WAIT_TIMEOUT;
  BOOL cancellation_valid = TRUE;
  for (int n = 0; n < 100 && wait == WAIT_TIMEOUT; ++n) {
    BOOL canceled = CancelIoEx(raw, NULL);
    if (!canceled && GetLastError() != ERROR_NOT_FOUND) { cancellation_valid = FALSE; break; }
    wait = WaitForSingleObject(reader, 10);
  }
  CHECK(cancellation_valid);
  CHECK(wait == WAIT_OBJECT_0 && state.result <= 0);
  CHECK(_close(fds[0]) == 0 && _close(fds[1]) == 0);
  CloseHandle(reader); CloseHandle(state.entered);
}

int main(void) {
  RoundTrip(); Cancellation();
  DWORD before = 0, after = 0;
  CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
  for (int n = 0; n < 100; ++n) {
    int fds[2]; CHECK(CreatePair(fds)); CHECK(_close(fds[0]) == 0 && _close(fds[1]) == 0);
  }
  CHECK(GetProcessHandleCount(GetCurrentProcess(), &after) && after <= before);
  printf("install_pipe_test all passed: %d checks\n", checks);
  return 0;
}
