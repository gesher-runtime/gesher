// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Default: exercise production rejection as an ordinary local user.
// --system-session ID: exercise the native launch as LocalSystem in a
// disposable Windows test environment with an ordinary desktop user logged on.
#include "../../compat/framework/app_container.h"
#include "../../compat/usb_storage/authority_launch.h"

#include <sddl.h>
#include <tlhelp32.h>
#include <wtsapi32.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <stdexcept>
#include <string>

namespace {
using gesher::usb_storage::AuthorityLaunch;
using gesher::usb_storage::AuthorityLaunchSpec;

constexpr DWORD kWait = 10000;

struct CloseHandleOwner {
  void operator()(void* handle) const { CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleOwner>;

void Require(bool value, const char* action, DWORD status = GetLastError()) {
  if (!value) throw std::runtime_error(std::string(action) + ": " + std::to_string(status));
}

struct User {
  alignas(TOKEN_USER) BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
  DWORD session = 0;

  explicit User(HANDLE token) {
    DWORD bytes = 0;
    Require(GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &bytes), "read test user");
    Require(GetTokenInformation(token, TokenSessionId, &session, sizeof(session), &bytes),
            "read test session");
  }
  PSID Sid() const { return reinterpret_cast<const TOKEN_USER*>(buffer)->User.Sid; }
};

struct Impersonation {
  explicit Impersonation(HANDLE token) {
    Require(ImpersonateLoggedOnUser(token), "impersonate test desktop user");
  }
  ~Impersonation() {
    if (!RevertToSelf()) std::abort();
  }
};

struct Profile {
  Profile(HANDLE owner, std::wstring value) : token(owner), name(std::move(value)) {
    const Impersonation identity(token);
    const DWORD status = gesher::appcontainer::EnsureProfile(name, name, &sid);
    Require(status == ERROR_SUCCESS, "create isolated AppContainer profile", status);
  }
  ~Profile() {
    if (!ImpersonateLoggedOnUser(token)) return;
    gesher::appcontainer::DeleteContainerProfile(name);
    if (!RevertToSelf()) std::abort();
  }
  HANDLE token;
  std::wstring name;
  gesher::appcontainer::Sid sid;
};

struct Binary {
  ~Binary() { if (*path) DeleteFileW(path); }
  wchar_t path[MAX_PATH]{};
};

std::wstring Environment() {
  wchar_t windows[MAX_PATH]{};
  const UINT count = GetWindowsDirectoryW(windows, MAX_PATH);
  Require(count != 0 && count < MAX_PATH, "get Windows directory");
  std::wstring environment = L"GESHER_AUTHORITY_QA=selected";
  environment.push_back(L'\0');
  environment.append(L"SystemRoot=").append(windows);
  environment.append(2, L'\0');
  return environment;
}

DWORD WINAPI SpareThread(void*) {
  Sleep(5000);
  return 0;
}

int Guest() {
  wchar_t chosen[24]{};
  if (GetEnvironmentVariableW(L"GESHER_AUTHORITY_QA", chosen, 24) != 8 ||
      std::wstring_view(chosen) != L"selected") return 21;
  if (GetEnvironmentVariableW(L"GESHER_AUTHORITY_PARENT_ONLY", chosen, 24) != 0 ||
      GetLastError() != ERROR_ENVVAR_NOT_FOUND) return 22;
  const Handle spare(CreateThread(nullptr, 0, SpareThread, nullptr, 0, nullptr));
  return spare && WaitForSingleObject(spare.get(), kWait) == WAIT_OBJECT_0 ? 0 : 23;
}

void CheckProcess(DWORD pid) {
  constexpr DWORD rights[] = {PROCESS_VM_WRITE, PROCESS_VM_OPERATION, PROCESS_CREATE_THREAD,
                              PROCESS_DUP_HANDLE, WRITE_DAC, WRITE_OWNER};
  for (DWORD right : rights) {
    const Handle forbidden(OpenProcess(right, FALSE, pid));
    Require(!forbidden && GetLastError() == ERROR_ACCESS_DENIED, "deny ordinary process access", right);
  }
}

void CheckThread(DWORD tid) {
  constexpr DWORD rights[] = {THREAD_SET_CONTEXT, THREAD_SUSPEND_RESUME, THREAD_SET_INFORMATION,
                              THREAD_IMPERSONATE, WRITE_DAC, WRITE_OWNER};
  for (DWORD right : rights) {
    const Handle forbidden(OpenThread(right, FALSE, tid));
    Require(!forbidden && GetLastError() == ERROR_ACCESS_DENIED, "deny ordinary thread access", right);
  }
}

DWORD SecondaryThread(DWORD process, DWORD primary) {
  const ULONGLONG deadline = GetTickCount64() + kWait;
  do {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    Require(snapshot != INVALID_HANDLE_VALUE, "enumerate native test threads");
    const Handle close(snapshot);
    THREADENTRY32 item{};
    item.dwSize = sizeof(item);
    if (Thread32First(snapshot, &item)) {
      do {
        if (item.th32OwnerProcessID == process && item.th32ThreadID != primary) {
          return item.th32ThreadID;
        }
        item.dwSize = sizeof(item);
      } while (Thread32Next(snapshot, &item));
    }
    Sleep(10);
  } while (GetTickCount64() < deadline);
  return 0;
}

void Positive(const wchar_t* executable, DWORD session) {
  HANDLE raw = nullptr;
  Require(WTSQueryUserToken(session, &raw), "get ordinary desktop test token from LocalSystem");
  Handle desktop(raw);
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  Require(GetTokenInformation(desktop.get(), TokenElevation, &elevation, sizeof(elevation), &size),
          "check desktop test token elevation");
  if (elevation.TokenIsElevated) {
    TOKEN_LINKED_TOKEN linked{};
    Require(GetTokenInformation(desktop.get(), TokenLinkedToken, &linked, sizeof(linked), &size),
            "get the limited desktop test token");
    desktop.reset(linked.LinkedToken);
  }
  const User user(desktop.get());
  Profile container(desktop.get(), L"GesherUsbAuthorityTest-" +
      std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  wchar_t temporary[MAX_PATH]{};
  const DWORD length = GetTempPathW(MAX_PATH, temporary);
  Require(length > 0 && length < MAX_PATH, "get test scratch directory");
  Binary copy;
  Require(GetTempFileNameW(temporary, L"gla", 0, copy.path), "reserve test image");
  Require(CopyFileW(executable, copy.path, FALSE), "copy standalone test image");
  for (PSID reader : {user.Sid(), container.sid.value}) {
    const DWORD status = gesher::appcontainer::Grant(copy.path, reader,
        FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false);
    Require(status == ERROR_SUCCESS, "grant scratch test image execution", status);
  }

  const std::wstring environment = Environment();
  const AuthorityLaunchSpec spec{copy.path, temporary, L"--guest", environment,
      {container.sid.value, nullptr, 0, 0}, user.Sid(), session};
  std::unique_ptr<AuthorityLaunch> launch;
  DWORD error = AuthorityLaunch::Create(desktop.get(), spec, &launch);
  Require(error == ERROR_SUCCESS && launch, "create protected AppContainer process", error);
  Require(WaitForSingleObject(launch->process(), 0) == WAIT_TIMEOUT, "leave test process suspended");
  {
    const Impersonation identity(desktop.get());
    CheckProcess(launch->pid());
    CheckThread(launch->main_thread_id());
  }
  error = launch->Resume();
  Require(error == ERROR_SUCCESS, "resume once", error);
  Require(launch->Resume() == ERROR_ALREADY_ASSIGNED, "reject repeated resume");
  const DWORD secondary = SecondaryThread(launch->pid(), launch->main_thread_id());
  Require(secondary != 0, "observe default-created guest thread");
  {
    const Impersonation identity(desktop.get());
    CheckThread(secondary);
  }
  Require(WaitForSingleObject(launch->process(), kWait) == WAIT_OBJECT_0, "wait for test guest");
  DWORD code = 0;
  Require(GetExitCodeProcess(launch->process(), &code) && code == 0, "run trusted environment witness", code);
  std::puts("USB authority launch: LocalSystem child and same-user rights checks passed");
}

void Negative(const wchar_t* executable) {
  HANDLE raw = nullptr;
  Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw),
          "open ordinary test token");
  const Handle desktop(raw);
  const User user(raw);
  Require(user.session != 0 && !IsWellKnownSid(user.Sid(), WinLocalSystemSid),
          "run ordinary negative mode in a desktop session");
  gesher::appcontainer::Sid package;
  Require(ConvertStringSidToSidW(L"S-1-15-2-1", &package.value), "create spec SID");
  package.local = true;
  const std::wstring environment = Environment();
  const AuthorityLaunchSpec spec{executable, L"C:\\Windows", L"--guest", environment,
      {package.value, nullptr, 0, 0}, user.Sid(), user.session};
  std::unique_ptr<AuthorityLaunch> launch;
  const DWORD denied = AuthorityLaunch::Create(desktop.get(), spec, &launch);
  Require(denied == ERROR_ACCESS_DENIED && !launch, "deny non-LocalSystem launcher", denied);
  AuthorityLaunchSpec invalid = spec;
  invalid.environment = {};
  Require(AuthorityLaunch::Create(desktop.get(), invalid, &launch) == ERROR_INVALID_PARAMETER &&
      !launch, "deny implicit launcher environment inheritance");
  std::wstring shadowed(1, L'\0');
  shadowed.append(environment);
  invalid.environment = shadowed;
  Require(AuthorityLaunch::Create(desktop.get(), invalid, &launch) == ERROR_INVALID_PARAMETER &&
      !launch, "deny entries hidden behind initial empty environment");
  std::puts("USB authority launch: desktop caller and incomplete environment rejected");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 2 && std::wstring_view(argv[1]) == L"--guest") return Guest();
  try {
    wchar_t executable[MAX_PATH]{};
    const DWORD size = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    Require(size && size < MAX_PATH, "get test executable");
    Require(SetEnvironmentVariableW(L"GESHER_AUTHORITY_PARENT_ONLY", L"parent"),
            "set test parent environment marker");
    if (argc == 3 && std::wstring_view(argv[1]) == L"--system-session") {
      wchar_t* end = nullptr;
      const unsigned long session = std::wcstoul(argv[2], &end, 10);
      Require(session > 0 && session < MAXDWORD && *end == L'\0', "parse test desktop session");
      Positive(executable, static_cast<DWORD>(session));
    } else if (argc == 1) {
      Negative(executable);
    } else {
      return 2;
    }
    return 0;
  } catch (const std::exception& failure) {
    std::fprintf(stderr, "USB authority launch: %s\n", failure.what());
    return 1;
  }
}
