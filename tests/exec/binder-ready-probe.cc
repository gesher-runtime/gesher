// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Exercise the startup handshake without AOSP: delayed, malformed and stalled
// brokers are separate Windows processes with actual named-pipe endpoints.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "services_zygote.h"
#include "posix/src/binder_wire.h"

namespace wire = gesher::binder_wire;
namespace {
std::wstring Wide(const char* text) { return std::wstring(text, text + std::strlen(text)); }
std::u16string U16(const std::wstring& value) {
  return {reinterpret_cast<const char16_t*>(value.data()), value.size()};
}
bool Transfer(HANDLE pipe, void* bytes, DWORD size, bool write) {
  auto* cursor = static_cast<unsigned char*>(bytes);
  while (size) {
    DWORD count = 0;
    if (!(write ? WriteFile(pipe, cursor, size, &count, nullptr) : ReadFile(pipe, cursor, size, &count, nullptr)) || !count) return false;
    cursor += count;
    size -= count;
  }
  return true;
}
int Server(const char* device, const char* mode) {
  if (!std::strcmp(mode, "exit")) return 0;
  if (!std::strcmp(mode, "delay")) Sleep(200);
  std::wstring name = wire::kPipePrefix + Wide(device);
  HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return 2;
  if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(pipe); return 3; }
  uint32_t hello[6]{};
  if (!Transfer(pipe, hello, sizeof(hello), false)) { CloseHandle(pipe); return 0; }
  if (hello[0] != 20 || hello[1] != wire::kHelloProc || hello[2] != wire::kMagic || hello[3] != wire::kVersion) return 4;
  if (!std::strcmp(mode, "stall")) { Sleep(5000); CloseHandle(pipe); return 0; }
  uint32_t reply[]{16, 0, 1, 42, 0};
  if (!std::strcmp(mode, "malformed")) reply[0] = 8;
  Transfer(pipe, reply, sizeof(reply), true);
  char end;
  DWORD read = 0;
  ReadFile(pipe, &end, 1, &read, nullptr);  // keep broker alive until the probe closes
  CloseHandle(pipe);
  return 0;
}
struct Child {
  HANDLE process = nullptr;
  DWORD pid = 0;
  ~Child() {
    if (process) {
      if (WaitForSingleObject(process, 1000) == WAIT_TIMEOUT) TerminateProcess(process, 137);
      WaitForSingleObject(process, 5000);
      CloseHandle(process);
    }
  }
  bool Start(const std::wstring& executable, const std::wstring& device, const char* mode) {
    std::wstring command = L"\"" + executable + L"\" --server " + device + L" " + Wide(mode);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &startup, &info)) return false;
    process = info.hProcess;
    pid = info.dwProcessId;
    CloseHandle(info.hThread);
    return true;
  }
};
}  // namespace
int main(int argc, char** argv) {
  if (argc == 4 && !std::strcmp(argv[1], "--server")) return Server(argv[2], argv[3]);
  wchar_t path[32768];
  DWORD length = GetModuleFileNameW(nullptr, path, 32768);
  if (!length || length >= 32768) return 2;
  std::wstring executable(path, length);
  unsigned failed = 0, count = 0;
  auto check = [&](const char* label, bool condition) {
    ++count;
    if (!condition) { ++failed; std::printf("FAIL binder readiness: %s\n", label); }
  };
  check("missing endpoint is bounded", !gesher::zygote::WaitForBinder(u"exec3-no-endpoint", GetCurrentProcessId(), 0));
  check("invalid deadline is rejected", !gesher::zygote::WaitForBinder(u"exec3-no-endpoint", GetCurrentProcessId(), -1));
  for (const char* mode : {"delay", "normal", "normal", "normal", "malformed", "stall", "exit", "wrong-pid"}) {
    std::wstring device = L"exec3-ready-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(count);
    Child child;
    if (!child.Start(executable, device, mode)) { check("start probe broker", false); continue; }
    bool success = !std::strcmp(mode, "delay") || !std::strcmp(mode, "normal");
    uint64_t start = GetTickCount64();
    bool result = gesher::zygote::WaitForBinder(U16(device), !std::strcmp(mode, "wrong-pid") ? GetCurrentProcessId() : child.pid,
                                               !std::strcmp(mode, "stall") ? 250 : 2000);
    check(mode, result == success && GetTickCount64() - start < 3000);
  }
  std::printf("exec3 binder readiness: %u checks, %u failed\n", count, failed);
  return failed ? 1 : 0;
}
