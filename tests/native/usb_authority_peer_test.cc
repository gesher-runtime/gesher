// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "../../compat/framework/app_container.h"
#include "../../compat/usb_storage/authority_peer.h"

#include <sddl.h>

#include <cstddef>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using gesher::usb_storage::AuthorityPeer;
constexpr DWORD kWait = 5000;

void Require(bool value, const char* action, DWORD status = GetLastError()) {
  if (!value) throw std::runtime_error(std::string(action) + ": " + std::to_string(status));
}

struct Process {
  PROCESS_INFORMATION info{};

  ~Process() {
    if (!info.hProcess) return;
    if (WaitForSingleObject(info.hProcess, 0) != WAIT_OBJECT_0) {
      TerminateProcess(info.hProcess, 1);
      WaitForSingleObject(info.hProcess, kWait);
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
  }
};

struct TestContainer {
  ~TestContainer() {
    if (sid.value) gesher::appcontainer::DeleteContainerProfile(name);
  }
  std::wstring name;
  gesher::appcontainer::Sid sid;
};

struct TempExecutable {
  ~TempExecutable() {
    if (*path) DeleteFileW(path);
  }
  wchar_t path[MAX_PATH]{};
};

void Start(const std::wstring& image, const std::wstring& name, PSID container,
           bool anonymous, Process* process) {
  std::wstring command = L"\"" + image + L"\" " + name + (anonymous ? L" anonymous" : L"");
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = container ? sizeof(startup) : sizeof(STARTUPINFOW);
  DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW;
  SIZE_T size = 0;
  std::vector<std::byte> attributes;
  SECURITY_CAPABILITIES capabilities{};
  if (container) {
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    Require(size != 0, "get AppContainer attribute list size");
    attributes.resize(size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    Require(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size),
            "initialize AppContainer attribute list");
    capabilities.AppContainerSid = container;
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &capabilities, sizeof(capabilities),
            nullptr, nullptr)) {
      const DWORD status = GetLastError();
      DeleteProcThreadAttributeList(startup.lpAttributeList);
      Require(false, "set AppContainer attributes", status);
    }
    flags |= EXTENDED_STARTUPINFO_PRESENT;
  }
  const BOOL created = CreateProcessW(image.c_str(), command.data(), nullptr, nullptr,
      FALSE, flags, nullptr, nullptr, &startup.StartupInfo, &process->info);
  const DWORD status = GetLastError();
  if (startup.lpAttributeList) DeleteProcThreadAttributeList(startup.lpAttributeList);
  Require(created, "create real child process", status);
}

void Resume(const Process& process) {
  Require(ResumeThread(process.info.hThread) == 1, "resume child");
}

void Finish(const Process& process) {
  Require(WaitForSingleObject(process.info.hProcess, kWait) == WAIT_OBJECT_0, "wait for child");
  DWORD code = 0;
  Require(GetExitCodeProcess(process.info.hProcess, &code), "read child exit code");
  Require(code == 0, "child pipe exchange", code);
}

enum class Exchange { kConnect, kRead, kWrite };

void Transfer(HANDLE pipe, Exchange operation, const Process* child = nullptr) {
  OVERLAPPED pending{};
  pending.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Require(pending.hEvent != nullptr, "create overlapped event");
  char byte = operation == Exchange::kWrite ? 'a' : 0;
  DWORD bytes = 0;
  BOOL ok;
  switch (operation) {
    case Exchange::kConnect:
      ok = ConnectNamedPipe(pipe, &pending);
      break;
    case Exchange::kRead:
      ok = ReadFile(pipe, &byte, 1, &bytes, &pending);
      break;
    case Exchange::kWrite:
      ok = WriteFile(pipe, &byte, 1, &bytes, &pending);
      break;
  }
  DWORD status = ok ? ERROR_SUCCESS : GetLastError();
  if (operation == Exchange::kConnect && status == ERROR_PIPE_CONNECTED) status = ERROR_SUCCESS;
  if (status == ERROR_IO_PENDING) {
    if (WaitForSingleObject(pending.hEvent, kWait) != WAIT_OBJECT_0) {
      CancelIoEx(pipe, &pending);
      GetOverlappedResult(pipe, &pending, &bytes, TRUE);
      status = ERROR_TIMEOUT;
    } else {
      status = GetOverlappedResult(pipe, &pending, &bytes, FALSE)
          ? ERROR_SUCCESS : GetLastError();
    }
  }
  CloseHandle(pending.hEvent);
  if (status == ERROR_TIMEOUT && child &&
      WaitForSingleObject(child->info.hProcess, 0) == WAIT_OBJECT_0) {
    DWORD code = 0;
    Require(GetExitCodeProcess(child->info.hProcess, &code), "read stopped client exit code");
    Require(false, "child exited before pipe exchange", code);
  }
  Require(status == ERROR_SUCCESS, "overlapped pipe exchange", status);
  if (operation == Exchange::kRead) Require(bytes == 1 && byte == 'q', "read child request");
  if (operation == Exchange::kWrite) Require(bytes == 1, "write child reply");
}

int RunClient(const wchar_t* name, bool anonymous) {
  HANDLE pipe = CreateFileW(name, GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, 0, nullptr,
      OPEN_EXISTING, SECURITY_SQOS_PRESENT |
          (anonymous ? SECURITY_ANONYMOUS : SECURITY_IDENTIFICATION), nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return static_cast<int>(GetLastError());
  char byte = 'q';
  DWORD bytes = 0;
  const bool success = WriteFile(pipe, &byte, 1, &bytes, nullptr) && bytes == 1 &&
      ReadFile(pipe, &byte, 1, &bytes, nullptr) && bytes == 1 && byte == 'a';
  CloseHandle(pipe);
  return success ? 0 : 1;
}

struct PipeSecurity {
  ~PipeSecurity() { LocalFree(attributes.lpSecurityDescriptor); }
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, FALSE};
};

void InitializeSecurity(PSID container, PipeSecurity* result) {
  HANDLE token = nullptr;
  Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), "open desktop test token");
  alignas(TOKEN_USER) std::byte user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  DWORD bytes = 0;
  const BOOL queried = GetTokenInformation(token, TokenUser, user, sizeof(user), &bytes);
  const DWORD status = GetLastError();
  CloseHandle(token);
  Require(queried, "read desktop test user", status);
  wchar_t* user_sid = nullptr;
  Require(ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(user)->User.Sid,
      &user_sid), "get desktop test SID string");
  wchar_t* container_sid = nullptr;
  if (!ConvertSidToStringSidW(container, &container_sid)) {
    const DWORD error = GetLastError();
    LocalFree(user_sid);
    Require(false, "get AppContainer SID string", error);
  }
  // The container can exchange data without permission to create another
  // instance of the server pipe.
  const std::wstring descriptor = std::wstring(L"D:P(A;;GA;;;") + user_sid +
      L")(A;;0x12018b;;;" + container_sid + L")S:(ML;;NW;;;LW)";
  LocalFree(container_sid);
  LocalFree(user_sid);
  Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(),
      SDDL_REVISION_1, &result->attributes.lpSecurityDescriptor, nullptr), "build pipe DACL");
}

void Test(const wchar_t* executable) {
  const std::wstring unique = std::to_wstring(GetCurrentProcessId()) + L"-" +
      std::to_wstring(GetTickCount64());
  TestContainer container;
  container.name = L"gesher-usb-authority-test-" + unique;
  DWORD status = gesher::appcontainer::EnsureProfile(container.name, container.name,
      &container.sid);
  Require(status == ERROR_SUCCESS, "create throwaway AppContainer", status);
  TempExecutable child;
  wchar_t temp[MAX_PATH]{};
  const DWORD size = GetTempPathW(MAX_PATH, temp);
  Require(size != 0 && size < MAX_PATH, "get scratch directory");
  Require(GetTempFileNameW(temp, L"gua", 0, child.path) != 0, "reserve scratch executable");
  Require(CopyFileW(executable, child.path, FALSE), "copy child to scratch");
  status = gesher::appcontainer::Grant(child.path, container.sid.value,
      FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false);
  Require(status == ERROR_SUCCESS, "grant scratch executable", status);

  PipeSecurity security;
  InitializeSecurity(container.sid.value, &security);
  const std::wstring name = LR"(\\.\pipe\gesher-usb-authority-test-)" + unique;
  std::unique_ptr<AuthorityPeer> peer;
  Process foreign;
  Start(child.path, name, nullptr, false, &foreign);
  status = AuthorityPeer::Create(foreign.info.hProcess, name.c_str(), &security.attributes, &peer);
  Require(status == ERROR_ACCESS_DENIED && !peer, "reject unrestricted process", status);

  Process selected;
  Start(child.path, name, container.sid.value, false, &selected);
  status = AuthorityPeer::Create(selected.info.hProcess, LR"(\\remote\pipe\not-a-local-server)",
      &security.attributes, &peer);
  Require(status == ERROR_INVALID_NAME && !peer, "reject remote endpoint name", status);
  status = AuthorityPeer::Create(selected.info.hProcess, name.c_str(), &security.attributes, &peer);
  Require(status == ERROR_SUCCESS && peer != nullptr, "create authority", status);

  Process sibling;
  Start(child.path, name, container.sid.value, false, &sibling);
  Resume(sibling);
  Transfer(peer->pipe(), Exchange::kConnect, &sibling);
  Transfer(peer->pipe(), Exchange::kRead);
  status = peer->Authenticate();
  Require(status == ERROR_ACCESS_DENIED, "reject second process in same container", status);
  Transfer(peer->pipe(), Exchange::kWrite);
  Finish(sibling);
  Require(DisconnectNamedPipe(peer->pipe()), "disconnect second container process");

  Resume(foreign);
  Transfer(peer->pipe(), Exchange::kConnect, &foreign);
  Transfer(peer->pipe(), Exchange::kRead);
  status = peer->Authenticate();
  Require(status == ERROR_ACCESS_DENIED, "reject second process from the same desktop user", status);
  Transfer(peer->pipe(), Exchange::kWrite);
  Finish(foreign);
  Require(DisconnectNamedPipe(peer->pipe()), "disconnect unrestricted process");

  Resume(selected);
  Transfer(peer->pipe(), Exchange::kConnect, &selected);
  Transfer(peer->pipe(), Exchange::kRead);
  Require(ImpersonateSelf(SecurityIdentification), "impersonate test authority worker");
  status = peer->Authenticate();
  HANDLE impersonation = nullptr;
  const BOOL still_impersonating = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &impersonation);
  if (still_impersonating) CloseHandle(impersonation);
  Require(RevertToSelf(), "restore test authority worker");
  Require(still_impersonating && status == ERROR_BAD_IMPERSONATION_LEVEL,
          "preserve pre-existing worker impersonation and reject request", status);
  status = peer->Authenticate();
  Require(status == ERROR_SUCCESS, "accept genuine launched AppContainer", status);
  const BOOL leaked_token = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &impersonation);
  const DWORD after = GetLastError();
  if (leaked_token) {
    CloseHandle(impersonation);
    RevertToSelf();
  }
  Require(!leaked_token && after == ERROR_NO_TOKEN, "restore server token after authentication", after);
  Transfer(peer->pipe(), Exchange::kWrite);
  Finish(selected);
  status = peer->Authenticate();
  Require(status == ERROR_PROCESS_ABORTED, "reject vanished process epoch", status);
  peer.reset();

  Process anonymous;
  Start(child.path, name, container.sid.value, true, &anonymous);
  status = AuthorityPeer::Create(anonymous.info.hProcess, name.c_str(), &security.attributes, &peer);
  Require(status == ERROR_SUCCESS && peer != nullptr, "create authority for anonymous test", status);
  Resume(anonymous);
  Transfer(peer->pipe(), Exchange::kConnect, &anonymous);
  Transfer(peer->pipe(), Exchange::kRead);
  status = peer->Authenticate();
  Require(status != ERROR_SUCCESS, "reject pipe token hidden by its expected process", status);
  Transfer(peer->pipe(), Exchange::kWrite);
  Finish(anonymous);
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc > 1) return RunClient(argv[1], argc > 2);
  try {
    wchar_t executable[MAX_PATH]{};
    const DWORD size = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    Require(size != 0 && size < MAX_PATH, "get test executable");
    Test(executable);
    std::puts("USB authority peer: real AppContainer, sibling, unrestricted, anonymous and exit checks passed");
    return 0;
  } catch (const std::exception& failure) {
    std::fprintf(stderr, "USB authority peer: %s\n", failure.what());
    return 1;
  }
}
