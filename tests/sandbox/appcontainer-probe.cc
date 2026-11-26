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

#include <cstdio>
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

bool WriteAll(const std::wstring& path, const char* text) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, CREATE_ALWAYS, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD size = static_cast<DWORD>(std::strlen(text)), written = 0;
  bool ok = WriteFile(file, text, size, &written, nullptr) && written == size;
  CloseHandle(file);
  return ok;
}

void StorageOpen(const std::wstring& base, const wchar_t* relative, const char* name, bool write) {
  std::wstring path = base + L"\\" + relative;
  HANDLE file = CreateFileW(path.c_str(), write ? GENERIC_WRITE : GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
  DWORD error = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
  if (file != INVALID_HANDLE_VALUE) {
    char byte = 'x';
    DWORD count = 0;
    BOOL ok = write ? WriteFile(file, &byte, 1, &count, nullptr)
                    : ReadFile(file, &byte, 1, &count, nullptr);
    if (!ok) error = GetLastError();
    CloseHandle(file);
  }
  std::printf("%s %s %lu\n", name, write ? "write" : "read", error);
}

// Stay in the same AppContainer process while the broker downgrades/revokes
// the ACL and replaces the policy file. Each phase opens fresh handles: ACL
// changes do not revoke handles that the kernel has already granted.
int StorageChild(const std::wstring& base, const std::wstring& control) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  int previous = 0;
  for (int attempt = 0; attempt < 2400; ++attempt) {
    int phase = std::atoi(ReadAll(control).c_str());
    if (phase > previous && phase <= 4) {
      std::printf("storage-phase %d\n", phase);
      const struct { const wchar_t* path; const char* name; } cases[] = {
          {L"media\\existing.txt", "existing"},
          {L"media\\new-top.txt", "new-top"},
          {L"media\\new-dir\\new-nested.txt", "new-nested"},
          {L"media\\Android\\data\\other\\secret.txt", "other-data"},
          {L"media\\Android\\obb\\other\\secret.txt", "other-obb"},
          {L"media\\Android\\data\\owner\\private.txt", "own-data"},
          {L"media\\Android\\obb\\owner\\private.txt", "own-obb"},
          {L"media\\Android\\media\\owner\\private.txt", "own-media"},
          {L"policy.txt", "policy"},
      };
      std::printf("policy-content %s\n", ReadAll(base + L"\\policy.txt").c_str());
      for (const auto& test : cases) {
        StorageOpen(base, test.path, test.name, false);
        StorageOpen(base, test.path, test.name, true);
      }
      std::printf("storage-done %d\n", phase);
      previous = phase;
      if (phase == 4) return 0;
    }
    Sleep(50);
  }
  std::printf("storage-child timed out\n");
  return 1;
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

int StorageProbe(const std::wstring& exe, const std::wstring& container,
                  const std::wstring& runtime, const std::wstring& base,
                  const std::wstring& log) {
  int failures = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
    return ok;
  };
  auto acl = [&](DWORD error, const char* what) {
    if (error != ERROR_SUCCESS) std::printf("Win32 error %lu\n", error);
    return check(error == ERROR_SUCCESS, what);
  };
  const DWORD read = 0x001200A9, modify = 0x001301BF;
  const std::wstring root = base + L"\\media";
  const std::wstring control = runtime + L"\\storage-phase.txt";
  const std::vector<std::wstring> directories{
      base, root, root + L"\\Android", root + L"\\Android\\data", root + L"\\Android\\obb",
      root + L"\\Android\\media", root + L"\\Android\\data\\other",
      root + L"\\Android\\obb\\other", root + L"\\Android\\data\\owner",
      root + L"\\Android\\obb\\owner", root + L"\\Android\\media\\owner"};
  for (const auto& dir : directories) {
    check(CreateDirectoryW(dir.c_str(), nullptr) != FALSE, "create isolated storage fixture directory");
  }
  const std::vector<std::wstring> files{
      root + L"\\existing.txt", root + L"\\Android\\data\\other\\secret.txt",
      root + L"\\Android\\obb\\other\\secret.txt", root + L"\\Android\\data\\owner\\private.txt",
      root + L"\\Android\\obb\\owner\\private.txt", root + L"\\Android\\media\\owner\\private.txt"};
  for (const auto& path : files) check(WriteAll(path, "data"), "create isolated storage fixture file");
  auto set = [&](const std::wstring& path, DWORD access, bool inherit) {
    return gesher::zygote::SetAppContainerStorageAccess(U16(path), U16(container), access, inherit);
  };
  // Seed a pre-existing inheritable shared grant to verify that protecting the
  // private roots removes it there while retaining the Windows owner's access.
  acl(set(root, modify, true), "seed shared Modify grant");
  for (const auto* sub : {L"data", L"obb"}) {
    acl(gesher::zygote::ProtectPrivateStorage(U16(root + L"\\Android\\" + sub)),
        "protect private storage from shared inheritance");
  }
  for (const auto* sub : {L"data", L"obb", L"media"}) {
    acl(set(root + L"\\Android\\" + sub + L"\\owner", modify, true), "grant own app directory");
  }
  acl(set(root, modify, true), "inheritable manager grant preserves protected private roots");
  // Model the original implementation's explicit child grant, which must be
  // cleaned up as well as downgrading the root.
  acl(gesher::zygote::GrantToAppContainer(U16(files[0]), U16(container), modify, false),
      "seed legacy explicit shared-child grant");
  check(WriteAll(root + L"\\new-top.txt", "new"), "host creates top-level file after manager grant");
  check(CreateDirectoryW((root + L"\\new-dir").c_str(), nullptr) != FALSE,
        "host creates top-level directory after manager grant");
  check(WriteAll(root + L"\\new-dir\\new-nested.txt", "new"),
        "host creates nested file after manager grant");
  auto publish = [&](const char* content) {
    std::wstring staged = base + L"\\policy.tmp", target = base + L"\\policy.txt";
    if (!check(WriteAll(staged, content), "write replacement policy inode")) return;
    if (!acl(set(staged, read, false), "prepare replacement policy inode ACL")) return;
    check(MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
              != FALSE, "atomically replace readable policy file");
  };
  publish("manager");
  DeleteFileW(log.c_str());
  gesher::zygote::AppContainerSpec spec{U16(container), U16(container), {u"gesherRuntimeFiles"}};
  std::wstring command = L"\"" + exe + L"\" --storage-child \"" + base + L"\" \"" + control + L"\"";
  int pid = failures == 0
                ? gesher::zygote::SpawnInAppContainer(U16(command), {}, {}, U16(log), spec) : -1;
  int child_exit = STILL_ACTIVE;
  int64_t child_identity = pid > 0 ? gesher::zygote::ChildStartTime(pid) : 0;
  if (pid <= 0) std::printf("storage child spawn/setup error %d\n", pid);
  check(pid > 0, "start storage access child with real AppContainer token");
  for (int phase = 1; phase <= 4 && pid > 0; ++phase) {
    if (phase == 2) {
      acl(set(root, read, true), "downgrade shared Modify to exactly Read");
      acl(set(files[0], 0, false), "remove legacy explicit child grant");
      publish("read-only");
    } else if (phase == 3) {
      acl(set(root, 0, false), "revoke shared grant from running app");
      publish("scoped");
    } else if (phase == 4) {
      for (const auto* sub : {L"data", L"obb", L"media"}) {
        acl(set(root + L"\\Android\\" + sub + L"\\owner", 0, false), "revoke uninstalled app's own grant");
      }
      publish("removed");
    }
    check(WriteAll(control, std::to_string(phase).c_str()), "signal access phase to the running process");
    std::string output, done = "storage-done " + std::to_string(phase) + "\n";
    for (int attempt = 0; attempt < 1200; ++attempt) {
      output = ReadAll(log);
      if (output.find(done) != std::string::npos) break;
      child_exit = gesher::zygote::ExitCode(pid);
      if (child_exit != STILL_ACTIVE) {
        output = ReadAll(log);
        break;
      }
      Sleep(50);
    }
    bool completed = check(output.find(done) != std::string::npos, "running child completes access phase");
    size_t start = output.find("storage-phase " + std::to_string(phase) + "\n");
    output = start == std::string::npos ? output : output.substr(start);
    std::printf("-- storage phase %d:\n%s", phase, output.c_str());
    if (!completed) break;
    auto expect_access = [&](const char* name, bool readable, bool writable) {
      for (int write = 0; write <= 1; ++write) {
        bool allowed = write ? writable : readable;
        std::string line = std::string(name) + (write ? " write " : " read ") +
                           (allowed ? "0\n" : "5\n");
        failures += !Expect(output, line.c_str(), line.substr(0, line.size() - 1).c_str());
      }
    };
    for (const auto* name : {"existing", "new-top", "new-nested"}) {
      expect_access(name, phase <= 2, phase == 1);
    }
    expect_access("other-data", false, false);
    expect_access("other-obb", false, false);
    for (const auto* name : {"own-data", "own-obb", "own-media"}) {
      expect_access(name, phase < 4, phase < 4);
    }
    expect_access("policy", true, false);
    const char* content[] = {"manager", "read-only", "scoped", "removed"};
    std::string line = std::string("policy-content ") + content[phase - 1] + "\n";
    failures += !Expect(output, line.c_str(), "running app reads the current replacement policy inode");
  }
  if (pid > 0) {
    // ExitCode consumes the retained handle once the process exits. Keep that
    // result instead of querying a second time and mistaking -1 for its status.
    for (int attempt = 0; attempt < 100 && child_exit == STILL_ACTIVE; ++attempt) {
      child_exit = gesher::zygote::ExitCode(pid);
      if (child_exit == STILL_ACTIVE) Sleep(10);
    }
    if (child_exit == STILL_ACTIVE) {
      gesher::zygote::KillChild(pid, child_identity);
      check(false, "storage child exits successfully");
    } else {
      check(child_exit == 0, "storage child exits successfully");
    }
  }
  // The Windows owner must still be able to clean up protected private roots.
  for (const auto& path : files) check(DeleteFileW(path.c_str()) != FALSE, "Windows owner retains fixture cleanup access");
  DeleteFileW((root + L"\\new-top.txt").c_str());
  DeleteFileW((root + L"\\new-dir\\new-nested.txt").c_str());
  RemoveDirectoryW((root + L"\\new-dir").c_str());
  DeleteFileW((base + L"\\policy.txt").c_str());
  DeleteFileW((base + L"\\policy.tmp").c_str());
  DeleteFileW(control.c_str());
  for (auto it = directories.rbegin(); it != directories.rend(); ++it) RemoveDirectoryW(it->c_str());
  return failures;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && !std::strcmp(argv[1], "--storage-child")) {
    return StorageChild(Wide(argv[2]), Wide(argv[3]));
  }
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

  failures += StorageProbe(exe, container, dir,
                            std::wstring(temp) + L"gesher-storage-probe-" + stamp, log);

  DeleteFileW(exe.c_str());
  RemoveDirectoryW(dir.c_str());
  gesher::zygote::DeleteAppContainer(U16(container));
  DeleteFileW(canary.c_str());
  DeleteFileW(log.c_str());
  if (failures == 0) std::printf("all passed\n");
  return failures == 0 ? 0 : 1;
}
