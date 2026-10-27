// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Transparent external transport-fault fixture. Every request and preceding
// response is forwarded unchanged to a real binderd. Only the first actual
// successful finish ACK is consumed here and withheld from the public shim.
#include <windows.h>
#include <atomic>
#include <thread>
#include "sg-actor.h"
#include "../../compat/posix/src/binder_wire.h"

namespace {
namespace wire = gesher::binder_wire;
std::atomic<bool> dropped{false};
std::wstring Pipe(const char* device) {
  std::wstring name = wire::kPipePrefix;
  for (const char* p = device; *p; ++p) name += static_cast<unsigned char>(*p);
  return name;
}
bool Exact(HANDLE pipe, void* bytes, size_t size, bool write) {
  auto* cursor = static_cast<uint8_t*>(bytes);
  while (size) {
    DWORD count = 0;
    DWORD want = static_cast<DWORD>(size > 65536 ? 65536 : size);
    if (!(write ? WriteFile(pipe, cursor, want, &count, nullptr) : ReadFile(pipe, cursor, want, &count, nullptr)) || count == 0) return false;
    cursor += count; size -= count;
  }
  return true;
}
bool Receive(HANDLE pipe, std::vector<uint8_t>* frame) {
  uint32_t length;
  if (!Exact(pipe, &length, sizeof(length), false) || length > (64u << 20)) return false;
  frame->resize(length);
  return Exact(pipe, frame->data(), frame->size(), false);
}
bool Send(HANDLE pipe, std::vector<uint8_t>& frame) {
  uint32_t length = static_cast<uint32_t>(frame.size());
  return Exact(pipe, &length, sizeof(length), true) && Exact(pipe, frame.data(), frame.size(), true);
}
bool IsAcceptedFinish(const std::vector<uint8_t>& request) {
  if (request.size() != 16) return false;
  uint32_t operation, accepted;
  memcpy(&operation, request.data(), sizeof(operation));
  memcpy(&accepted, request.data() + 12, sizeof(accepted));
  return operation == wire::kFinishBuffer && accepted == 1;
}
void Relay(HANDLE front, std::wstring backend_name, std::string prefix) {
  HANDLE backend = INVALID_HANDLE_VALUE;
  for (unsigned i = 0; i < 1500; ++i) {
    backend = CreateFileW(backend_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (backend != INVALID_HANDLE_VALUE) break;
    Sleep(20);
  }
  if (backend != INVALID_HANDLE_VALUE) {
    std::vector<uint8_t> request, response;
    while (Receive(front, &request) && Send(backend, request) && Receive(backend, &response)) {
      int32_t status = -1;
      if (response.size() == sizeof(status)) memcpy(&status, response.data(), sizeof(status));
      bool expected = false;
      if (IsAcceptedFinish(request) && response.size() == 4 && status == 0 &&
          dropped.compare_exchange_strong(expected, true)) {
        // Real broker finish has completed. All transaction bytes have already
        // reached the real client before this actual ACK is deliberately lost.
        if (!binder_sg_fixture::Mark(prefix + ".ack-dropped")) {
          fprintf(stderr, "FAIL SG relay: cannot record actual finish ACK\n");
        } else {
          printf("PASS SG relay: actual broker length4/status0 finish ACK verified and withheld after unchanged transaction bytes\n");
          fflush(stdout);
        }
        break;
      }
      if (!Send(front, response)) break;
    }
    CloseHandle(backend);
  }
  DisconnectNamedPipe(front);
  CloseHandle(front);
}
}  // namespace
int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc != 4) return 2;
  const std::wstring front_name = Pipe(argv[1]), backend_name = Pipe(argv[2]);
  const std::string prefix = argv[3];
  bool first = true;
  for (;;) {
    HANDLE pipe = CreateNamedPipeW(front_name.c_str(), PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 3;
    if (first && !binder_sg_fixture::Mark(prefix + ".proxy-ready")) { CloseHandle(pipe); return 4; }
    first = false;
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(pipe); continue; }
    std::thread(Relay, pipe, backend_name, prefix).detach();
  }
}
