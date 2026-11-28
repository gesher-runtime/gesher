// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "../../compat/binder/protected_peers.h"

#include <aclapi.h>
#include <sddl.h>

#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

using gesher::binder::ProtectedPeers;
using gesher::binder::PeerRole;
constexpr DWORD kTimeout = 3000;

void Require(bool condition, const char* operation, DWORD error = GetLastError()) {
  if (!condition) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(error));
}

struct FileHandle {
  HANDLE value;
  ~FileHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct Process {
  PROCESS_INFORMATION info{};
  ~Process() {
    if (!info.hProcess) return;
    if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) {
      TerminateProcess(info.hProcess, 1);
      WaitForSingleObject(info.hProcess, kTimeout);
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
  }
};

struct Security {
  ~Security() { LocalFree(attributes.lpSecurityDescriptor); }
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, FALSE};
};

std::wstring SidString(PSID sid) {
  wchar_t* value = nullptr;
  Require(ConvertSidToStringSidW(sid, &value) != FALSE, "format test owner's SID");
  const std::wstring result(value);
  LocalFree(value);
  return result;
}

void SetSecurity(PSID owner, bool broad, Security* security) {
  // The same owner-only DACL shape as the protected launcher, using the
  // fixture's actual account to make a standard-token test possible.
  const std::wstring sid = SidString(owner);
  const std::wstring sddl = L"O:" + sid + L"D:P(A;;0x001fffff;;;" + sid + L")" +
      (broad ? L"(A;;0x40;;;WD)" : L"");
  Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
      &security->attributes.lpSecurityDescriptor, nullptr) != FALSE, "build process DACL");
}

void Start(const std::wstring& pipe, SECURITY_ATTRIBUTES* attributes, Process* process) {
  wchar_t image[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, image, MAX_PATH);
  Require(length != 0 && length < MAX_PATH, "resolve test image");
  std::wstring command = L"\"" + std::wstring(image, length) + L"\" --child " + pipe;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  Require(CreateProcessW(image, command.data(), attributes, nullptr, FALSE,
                         CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
                         &startup, &process->info) != FALSE, "start real suspended process");
}

void AwaitConnection(HANDLE pipe, const Process& child) {
  FileHandle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  Require(ready.value != nullptr, "create pipe wait event");
  OVERLAPPED connect{};
  connect.hEvent = ready.value;
  const BOOL connected = ConnectNamedPipe(pipe, &connect);
  const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
  if (error == ERROR_SUCCESS || error == ERROR_PIPE_CONNECTED) return;
  Require(error == ERROR_IO_PENDING, "start connecting real pipe", error);
  const HANDLE waits[] = {ready.value, child.info.hProcess};
  const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, kTimeout);
  DWORD ignored = 0;
  if (wait != WAIT_OBJECT_0) {
    CancelIoEx(pipe, &connect);
    GetOverlappedResult(pipe, &connect, &ignored, TRUE);
    Require(false, "wait for process pipe connection", wait);
  }
  Require(GetOverlappedResult(pipe, &connect, &ignored, FALSE) != FALSE, "complete pipe connection");
}

void DisconnectAndFinish(HANDLE pipe, const Process& child) {
  Require(DisconnectNamedPipe(pipe) != FALSE, "disconnect test client");
  Require(WaitForSingleObject(child.info.hProcess, kTimeout) == WAIT_OBJECT_0, "wait for test client");
  DWORD code = 0;
  Require(GetExitCodeProcess(child.info.hProcess, &code) && code == 0, "check client completion", code);
}

int RunClient(const wchar_t* name) {
  FileHandle pipe{CreateFileW(name, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr)};
  if (pipe.value == INVALID_HANDLE_VALUE) return 2;
  // Connect without any trusted claimed PID or UID. Wait while the server
  // resolves the kernel's connected process and examines the retained object.
  char byte = 0;
  DWORD bytes = 0;
  if (ReadFile(pipe.value, &byte, 1, &bytes, nullptr)) return 3;
  const DWORD error = GetLastError();
  return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ? 0 : 4;
}

void CheckTransfer(const ProtectedPeers::Peer& peer) {
  FileHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  Require(event.value != nullptr, "create transfer marker");
  HANDLE remote = nullptr;
  Require(DuplicateHandle(GetCurrentProcess(), event.value, peer->process(), &remote,
                         SYNCHRONIZE, FALSE, 0) != FALSE, "transfer to registered process");
  FileHandle returned{nullptr};
  Require(DuplicateHandle(peer->process(), remote, GetCurrentProcess(), &returned.value,
                         0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE) != FALSE,
          "recover handle from registered process");
  Require(SetEvent(event.value) && WaitForSingleObject(returned.value, 0) == WAIT_OBJECT_0,
          "transferred the same native object");
}

void Run() {
  HANDLE raw_token = nullptr;
  Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) != FALSE, "open test token");
  const FileHandle token{raw_token};
  alignas(TOKEN_USER) std::byte user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  DWORD bytes = 0;
  Require(GetTokenInformation(token.value, TokenUser, user, sizeof(user), &bytes) != FALSE,
          "query test process owner");
  PSID owner = reinterpret_cast<const TOKEN_USER*>(user)->User.Sid;
  Require(!IsWellKnownSid(owner, WinLocalSystemSid), "run as an ordinary test account");
  Security restricted, broad;
  SetSecurity(owner, false, &restricted);
  SetSecurity(owner, true, &broad);
  const std::wstring name = LR"(\\.\pipe\gesher-test-binder-peer-)" +
      std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
  FileHandle pipe{CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
      FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1,
      4096, 4096, 0, nullptr)};
  Require(pipe.value != INVALID_HANDLE_VALUE, "create real server pipe");

  ProtectedPeers peers;
  Process selected, unrelated, wrong_acl;
  Start(name, &restricted.attributes, &selected);
  Start(name, &restricted.attributes, &unrelated);
  Start(name, &broad.attributes, &wrong_acl);
  ProtectedPeers::Peer registered, result;
  Require(peers.Register(selected.info.hProcess, 12345, PeerRole::kClient, &result) ==
              ERROR_ACCESS_DENIED && !result,
          "production rejects test user as process owner");
  Require(peers.RegisterForFixture(wrong_acl.info.hProcess, 12345, PeerRole::kClient, owner, &result) ==
          ERROR_ACCESS_DENIED && !result, "reject nonexclusive process DACL");
  const DWORD error = peers.RegisterForFixture(selected.info.hProcess, 12345, PeerRole::kClient,
                                                owner, &registered);
  Require(error == ERROR_SUCCESS, "register actual suspended process", error);
  FILETIME created{}, exited{}, kernel{}, user_time{};
  Require(GetProcessTimes(selected.info.hProcess, &created, &exited, &kernel, &user_time) != FALSE,
          "read launch handle's actual epoch");
  const uint64_t epoch = (uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime;
  Require(registered->pid() == selected.info.dwProcessId && registered->uid() == 12345 &&
          epoch != 0 && registered->creation_time() == epoch && registered->live(),
          "verify registered process epoch");
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  PSID unexpected_owner = nullptr;
  const DWORD visibility = GetSecurityInfo(registered->process(), SE_KERNEL_OBJECT,
      OWNER_SECURITY_INFORMATION, &unexpected_owner, nullptr, nullptr, nullptr, &descriptor);
  LocalFree(descriptor);
  Require(visibility == ERROR_ACCESS_DENIED, "retained handle has no READ_CONTROL", visibility);
  Require(peers.RegisterForFixture(selected.info.hProcess, 23456, PeerRole::kClient, owner, &result) ==
          ERROR_ALREADY_EXISTS && !result, "reject a second UID for the same process epoch");

  Require(ResumeThread(unrelated.info.hThread) == 1, "resume unregistered process");
  AwaitConnection(pipe.value, unrelated);
  Require(peers.FindClient(pipe.value, &result) == ERROR_ACCESS_DENIED && !result,
          "reject an unrelated process with the same owner/DACL");
  DisconnectAndFinish(pipe.value, unrelated);

  Require(ResumeThread(selected.info.hThread) == 1, "resume registered process");
  AwaitConnection(pipe.value, selected);
  const DWORD lookup = peers.FindClient(pipe.value, &result);
  if (lookup != ERROR_SUCCESS) {
    ULONG pid = 0, session = 0;
    const BOOL known_pid = GetNamedPipeClientProcessId(pipe.value, &pid);
    const BOOL known_session = GetNamedPipeClientSessionId(pipe.value, &session);
    std::fprintf(stderr, "client pid=%lu (%d), session=%lu (%d), expected pid=%lu session=%lu\n",
        pid, known_pid, session, known_session, registered->pid(), registered->session());
  }
  Require(lookup == ERROR_SUCCESS && result == registered,
          "resolve real pipe client to its registered UID", lookup);
  CheckTransfer(result);
  result.reset();
  peers.Revoke(registered);
  Require(!registered->live() && peers.FindClient(pipe.value, &result) == ERROR_ACCESS_DENIED && !result,
          "deny admission after revocation");
  Require(peers.RegisterForFixture(selected.info.hProcess, 23456, PeerRole::kClient, owner, &result) ==
          ERROR_ALREADY_EXISTS && !result, "do not rebind a revoked live process");
  CheckTransfer(registered);  // Outstanding operations still own their original process object.
  DisconnectAndFinish(pipe.value, selected);
  Require(WaitForSingleObject(registered->process(), 0) == WAIT_OBJECT_0,
          "retained process object reports its original exit");
  Require(peers.RegisterForFixture(selected.info.hProcess, 12345, PeerRole::kClient, owner, &result) ==
          ERROR_PROCESS_ABORTED && !result, "reject a stale process object");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wstring(argv[1]) == L"--child") return RunClient(argv[2]);
  try {
    Run();
    std::puts("Protected Binder peer ACL, process epoch, pipe identity, and transfer checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
