// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// AppContainer isolation, milestone M1 (docs/decisions/0090-appcontainer-app-isolation.md):
// the production launch path (gesher::zygote::SpawnInAppContainer) and binderd's
// sandbox admission, with real Windows processes, tokens and a running binderd.
//
//   appcontainer-probe <binder device>
//
// binderd must serve <device> with GESHER_SANDBOX=appcontainer. The probe starts
// copies of itself in a throwaway AppContainer, with and without the binder-client
// capability, and each reports what the kernel and binderd let it do. Prints
// "all passed" and exits 0 when every expectation holds.
#include <windows.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app_container.h"
#include "posix/src/binder_wire.h"
#include "services_zygote.h"

namespace wire = gesher::binder_wire;
namespace {

std::u16string U16(const std::wstring& value) {
  return {reinterpret_cast<const char16_t*>(value.data()), value.size()};
}

std::wstring Wide(const char* text) { return std::wstring(text, text + std::strlen(text)); }

bool Transfer(HANDLE pipe, void* bytes, DWORD size, bool write) {
  auto* cursor = static_cast<unsigned char*>(bytes);
  while (size) {
    DWORD count = 0;
    if (!(write ? WriteFile(pipe, cursor, size, &count, nullptr)
                : ReadFile(pipe, cursor, size, &count, nullptr)) ||
        !count) {
      return false;
    }
    cursor += count;
    size -= count;
  }
  return true;
}

bool EventLoopDescriptors() {
  const int event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (event < 0) std::printf("event-loop: eventfd errno=%d\n", errno);
  const int poller = epoll_create1(EPOLL_CLOEXEC);
  if (poller < 0) std::printf("event-loop: epoll_create1 errno=%d\n", errno);
  const int alias = event >= 0 ? dup(event) : -1;
  if (event >= 0 && alias < 0) std::printf("event-loop: dup errno=%d\n", errno);
  epoll_event interest{};
  interest.events = EPOLLIN;
  interest.data.u64 = 19;
  epoll_event ready{};
  eventfd_t value = 0;
  const char* failure = nullptr;
  if (event < 0 || poller < 0 || alias < 0) failure = "open";
  else if (epoll_ctl(poller, EPOLL_CTL_ADD, event, &interest) != 0) failure = "epoll_ctl";
  else if (eventfd_write(alias, 1) != 0) failure = "eventfd_write";
  else if (epoll_wait(poller, &ready, 1, 1000) != 1 ||
           (ready.events & EPOLLIN) == 0 || ready.data.u64 != 19) failure = "epoll_wait";
  else if (eventfd_read(event, &value) != 0 || value != 1) failure = "eventfd_read";
  if (failure != nullptr) std::printf("event-loop: %s errno=%d\n", failure, errno);
  if (alias >= 0) close(alias);
  if (event >= 0) close(event);
  if (poller >= 0) close(poller);
  return failure == nullptr;
}

// Opens the device's pipe and says hello as (pid, uid). Returns the reply's status,
// or a description of how the connection failed.
std::string Hello(const std::wstring& device, uint32_t pid, uint32_t uid) {
  std::wstring name = wire::kPipePrefix + device;
  HANDLE pipe = CreateFileW(name.c_str(), wire::kPipeClientAccess, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return "open error " + std::to_string(GetLastError());
  uint32_t hello[]{5 * sizeof(uint32_t), wire::kHelloProc, wire::kMagic, wire::kVersion, pid, uid};
  uint32_t length = 0;
  int32_t status = 1;
  std::string result;
  if (!Transfer(pipe, hello, sizeof(hello), true) ||
      !Transfer(pipe, &length, sizeof(length), false) || length < sizeof(status) ||
      !Transfer(pipe, &status, sizeof(status), false)) {
    result = "io error " + std::to_string(GetLastError());
  } else {
    result = "status " + std::to_string(status);
  }
  CloseHandle(pipe);
  return result;
}

// In the container: what it may do. One line per check: "<check> <result>".
int Child(const std::wstring& device, const std::wstring& canary, DWORD parent) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // every line reaches the log, even on a crash
  close(STDIN_FILENO);
  std::printf("event-loop %s\n", EventLoopDescriptors() ? "ready" : "failed");
  HANDLE file = CreateFileW(canary.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    std::printf("canary opened\n");
    CloseHandle(file);
  } else {
    std::printf("canary error %lu\n", GetLastError());
  }
  // Serving an instance of the device's pipe (FILE_CREATE_PIPE_INSTANCE) is not granted.
  HANDLE server = CreateNamedPipeW((wire::kPipePrefix + device).c_str(), PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
  if (server != INVALID_HANDLE_VALUE) {
    std::printf("serve-instance created\n");
    CloseHandle(server);
  } else {
    std::printf("serve-instance error %lu\n", GetLastError());
  }
  const DWORD self = GetCurrentProcessId();
  std::printf("hello-app %s\n", Hello(device, self, 10123).c_str());
  std::printf("hello-system %s\n", Hello(device, self, 1000).c_str());
  std::printf("hello-other-pid %s\n", Hello(device, parent, 10123).c_str());
  std::fflush(stdout);
  return 0;
}

std::string ReadAll(const std::wstring& path) {
  std::string text;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return text;
  char buffer[4096];
  DWORD count = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &count, nullptr) && count) text.append(buffer, count);
  CloseHandle(file);
  return text;
}

// Starts this program in the container with `capabilities`; returns its output.
std::string RunContained(const std::wstring& exe, const std::wstring& container,
                         const std::vector<std::u16string>& capabilities,
                         const std::wstring& device, const std::wstring& canary,
                         const std::wstring& log) {
  DeleteFileW(log.c_str());
  gesher::zygote::AppContainerSpec spec{U16(container), U16(container), capabilities};
  std::wstring command = L"\"" + exe + L"\" --child " + device + L" \"" + canary + L"\" " +
                         std::to_wstring(GetCurrentProcessId());
  int pid = gesher::zygote::SpawnInAppContainer(U16(command), {}, {}, U16(log), spec);
  if (pid <= 0) return "spawn error " + std::to_string(-pid) + "\n";
  // A freshly built probe's first start can be slow (an antivirus scan): wait long,
  // and say so rather than return an empty log.
  for (int i = 0; i < 1200 && gesher::zygote::ExitCode(pid) == STILL_ACTIVE; ++i) Sleep(100);
  if (gesher::zygote::ExitCode(pid) == STILL_ACTIVE) {
    gesher::zygote::Kill(pid);
    return ReadAll(log) + "timed out after 120 s\n";
  }
  return ReadAll(log);
}

// Starts this program outside any container, at Low integrity (the pipe's label
// admits Low); returns its output.
std::string RunLowIntegrity(const std::wstring& exe, const std::wstring& device,
                            const std::wstring& canary, const std::wstring& log) {
  DeleteFileW(log.c_str());
  HANDLE self = nullptr, token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY |
                        TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY, &self)) {
    return "token error " + std::to_string(GetLastError()) + "\n";
  }
  BOOL ok = DuplicateTokenEx(self, 0, nullptr, SecurityImpersonation, TokenPrimary, &token);
  CloseHandle(self);
  if (!ok) return "duplicate error " + std::to_string(GetLastError()) + "\n";
  SID_IDENTIFIER_AUTHORITY mandatory = SECURITY_MANDATORY_LABEL_AUTHORITY;
  PSID low = nullptr;
  if (!AllocateAndInitializeSid(&mandatory, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0,
                                &low)) {
    CloseHandle(token);
    return "sid error " + std::to_string(GetLastError()) + "\n";
  }
  TOKEN_MANDATORY_LABEL label{};
  label.Label.Attributes = SE_GROUP_INTEGRITY;
  label.Label.Sid = low;
  ok = SetTokenInformation(token, TokenIntegrityLevel, &label,
                           sizeof(label) + GetLengthSid(low));
  FreeSid(low);
  if (!ok) {
    CloseHandle(token);
    return "integrity error " + std::to_string(GetLastError()) + "\n";
  }
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE out = CreateFileW(log.c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS, 0, nullptr);
  if (out == INVALID_HANDLE_VALUE) {
    DWORD failed = GetLastError();
    CloseHandle(token);
    return "log error " + std::to_string(failed) + "\n";
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = out;
  si.hStdError = out;
  std::wstring command = L"\"" + exe + L"\" --child " + device + L" \"" + canary + L"\" " +
                         std::to_wstring(GetCurrentProcessId());
  PROCESS_INFORMATION pi{};
  ok = CreateProcessAsUserW(token, nullptr, command.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  DWORD error = GetLastError();
  CloseHandle(token);
  CloseHandle(out);
  if (!ok) return "spawn error " + std::to_string(error) + "\n";
  if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0) {
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ReadAll(log) + "timed out after 120 s\n";
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return ReadAll(log);
}

bool Expect(const std::string& output, const char* line, const char* what) {
  bool ok = output.find(line) != std::string::npos;
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 5 && !std::strcmp(argv[1], "--child")) {
    return Child(Wide(argv[2]), Wide(argv[3]), static_cast<DWORD>(std::strtoul(argv[4], nullptr, 10)));
  }
  if (argc != 2) {
    std::fprintf(stderr, "usage: appcontainer-probe <binder device>\n");
    return 2;
  }
  const std::wstring device = Wide(argv[1]);
  wchar_t self[MAX_PATH], temp[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  GetTempPathW(MAX_PATH, temp);
  const std::wstring stamp = std::to_wstring(GetCurrentProcessId());
  const std::wstring container = L"Gesher.Test.Probe-" + stamp;
  // The user's own file: no ACE names a container, so the kernel must refuse it.
  const std::wstring canary = std::wstring(temp) + L"gesher-sandbox-canary-" + stamp + L".txt";
  HANDLE file = CreateFileW(canary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return 3;
  DWORD written = 0;
  WriteFile(file, "secret", 6, &written, nullptr);
  CloseHandle(file);
  // The children run a copy of this (statically linked) probe from a directory of
  // their own, opened to the runtime capability as the system server opens the
  // runtime's: the layout the probe came from is left as it is. The log is an
  // inherited handle.
  const std::wstring dir = std::wstring(temp) + L"gesher-sandbox-probe-" + stamp;
  const std::wstring exe = dir + L"\\appcontainer-probe.exe";
  if (!CreateDirectoryW(dir.c_str(), nullptr) || !CopyFileW(self, exe.c_str(), FALSE)) {
    std::printf("FAIL  copying the probe to %ls: %lu\n", dir.c_str(), GetLastError());
    RemoveDirectoryW(dir.c_str());
    DeleteFileW(canary.c_str());
    return 4;
  }
  int failures = 0;
  if (gesher::zygote::GrantToCapability(U16(dir), u"gesherRuntimeFiles", 0x001200A9, true) != 0) {
    std::printf("FAIL  granting the runtime capability on %ls\n", dir.c_str());
    ++failures;
  }
  const std::wstring log = std::wstring(temp) + L"gesher-sandbox-probe-" + stamp + L".log";

  std::string admitted = RunContained(exe, container, {u"gesherRuntimeFiles", u"gesherBinderClient"},
                                      device, canary, log);
  std::printf("-- contained, with gesherBinderClient:\n%s", admitted.c_str());
  failures += !Expect(admitted, "canary error 5", "the user's file is closed to the container (ERROR_ACCESS_DENIED)");
  failures += !Expect(admitted, "event-loop ready",
                      "eventfd, duplicate and epoll work inside the AppContainer after closing stdin");
  failures += !Expect(admitted, "hello-app status 0", "binderd admits the container as an app UID at its own pid");
  failures += !Expect(admitted, "hello-system status -1", "binderd refuses the container's claim of SYSTEM_UID (EPERM)");
  failures += !Expect(admitted, "hello-other-pid status -1", "binderd refuses the container's claim of another pid (EPERM)");
  failures += !Expect(admitted, "serve-instance error 5", "a container cannot serve an instance of the device's pipe");

  std::string refused = RunContained(exe, container, {u"gesherRuntimeFiles"}, device, canary, log);
  std::printf("-- contained, without gesherBinderClient:\n%s", refused.c_str());
  failures += !Expect(refused, "hello-app open error 5", "binderd's pipe is closed to a container without the capability");

  // The pipe's Low label admits Low integrity; outside a container it must not
  // speak for the system either.
  std::string low = RunLowIntegrity(exe, device, canary, log);
  std::printf("-- Low integrity, not contained:\n%s", low.c_str());
  failures += !Expect(low, "hello-app status 0", "binderd admits a Low integrity process as an app UID at its own pid");
  failures += !Expect(low, "hello-system status -1", "binderd refuses a Low integrity process's claim of SYSTEM_UID (EPERM)");

  // This process: the user's Medium token, as the system server's. Sandbox mode
  // must still admit it with SYSTEM_UID.
  std::string broker = "hello-system " + Hello(device, GetCurrentProcessId(), 1000) + "\n";
  std::printf("-- the user's own token:\n%s", broker.c_str());
  failures += !Expect(broker, "hello-system status 0", "binderd admits an uncontained Medium process with SYSTEM_UID");

  DeleteFileW(exe.c_str());
  RemoveDirectoryW(dir.c_str());
  gesher::zygote::DeleteAppContainer(U16(container));
  DeleteFileW(canary.c_str());
  DeleteFileW(log.c_str());
  if (failures == 0) std::printf("all passed\n");
  return failures == 0 ? 0 : 1;
}
