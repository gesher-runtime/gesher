// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <sddl.h>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "../../compat/binder/protected_data_client.h"
#include "../../compat/binder/protected_data_pipe.h"
#include "../../compat/binder/protected_client_startup.h"
#include "../../compat/posix/src/binder_transport.h"
#include "../../compat/framework/app_container.h"
#include "../../compat/usb_storage/binder_listener_factory.h"

namespace {
using gesher::usb_storage::BinderListenerFactory;
using gesher::binder::ProtectedDataEndpoint;
using gesher::binder::OpenProtectedDataPipeForFixture;

constexpr DWORD client_access = gesher::binder::data_pipe::kClientAccess;
constexpr DWORD pipe_modes = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
constexpr DWORD iterations = 3;
constexpr DWORD pipe_buffer = 1 << 16;
struct Closer {
  void operator()(void* h) const { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
using Handle = std::unique_ptr<void, Closer>;
void Require(bool condition, const char* message, DWORD code = GetLastError()) {
  if (!condition) throw std::runtime_error(std::string(message) + ": " + std::to_string(code));
}
struct Process {
  PROCESS_INFORMATION info{};
  ~Process() {
    const Handle thread(info.hThread), process(info.hProcess);
    if (process && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT) {
      TerminateProcess(process.get(), 1);
      WaitForSingleObject(process.get(), 5000);
    }
  }
  void Finish(const char* label, DWORD expected = 0) {
    Require(WaitForSingleObject(info.hProcess, 5000) == WAIT_OBJECT_0, label);
    DWORD status = MAXDWORD;
    Require(GetExitCodeProcess(info.hProcess, &status) && status == expected, label, status);
  }
};
struct Profile {
  std::wstring name;
  gesher::appcontainer::Sid sid;
  ~Profile() { if (sid.value) gesher::appcontainer::DeleteContainerProfile(name); }
};
struct User {
  User() {
    HANDLE raw = nullptr;
    Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw), "open test host token");
    const Handle token(raw);
    DWORD size = 0;
    Require(GetTokenInformation(raw, TokenUser, data, sizeof(data), &size), "read host SID");
  }

  PSID sid() const { return reinterpret_cast<const TOKEN_USER*>(data)->User.Sid; }

  alignas(TOKEN_USER) BYTE data[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
};

void PrepareProfile(const wchar_t* image, Profile* profile) {
  const DWORD status = gesher::appcontainer::EnsureProfile(profile->name, profile->name, &profile->sid);
  Require(status == ERROR_SUCCESS, "create isolated AppContainer", status);
  const DWORD granted = gesher::appcontainer::Grant(image, profile->sid.value,
      FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false);
  Require(granted == ERROR_SUCCESS, "allow scratch executable in AppContainer", granted);
}

void Spawn(const wchar_t* image, std::wstring args, Process* child,
           PSID container = nullptr, HANDLE inherit = nullptr) {
  std::wstring command = std::wstring(L"\"") + image + L"\" " + args;
  STARTUPINFOEXW setup{};
  setup.StartupInfo.cb = sizeof(setup);
  SIZE_T bytes = 0;
  const DWORD count = (container ? 1 : 0) + (inherit ? 1 : 0);
  InitializeProcThreadAttributeList(nullptr, count, 0, &bytes);
  std::vector<BYTE> storage(bytes);
  setup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  Require(InitializeProcThreadAttributeList(setup.lpAttributeList, count, 0, &bytes), "initialize attributes");
  SECURITY_CAPABILITIES security{container, nullptr, 0, 0};
  const bool attribute = (!container || UpdateProcThreadAttribute(setup.lpAttributeList, 0,
          PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &security, sizeof(security), nullptr, nullptr)) &&
      (!inherit || UpdateProcThreadAttribute(setup.lpAttributeList, 0,
          PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inherit, sizeof(inherit), nullptr, nullptr));
  const bool launched = attribute && CreateProcessW(image, command.data(), nullptr, nullptr,
      inherit != nullptr, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
      &setup.StartupInfo, &child->info);
  const DWORD status = GetLastError();
  DeleteProcThreadAttributeList(setup.lpAttributeList);
  Require(launched, "start real factory test process", status);
}
template <typename T>
bool Read(HANDLE h, T* result) {
  DWORD bytes = 0;
  return ReadFile(h, result, sizeof(T), &bytes, nullptr) && bytes == sizeof(T);
}
template <typename T>
bool Write(HANDLE h, const T& value) {
  DWORD bytes = 0;
  return WriteFile(h, &value, sizeof(T), &bytes, nullptr) && bytes == sizeof(T);
}
struct Observed { ULONG opener; ULONG worker; };
ProtectedDataEndpoint Endpoint(std::wstring_view locator, DWORD creator) {
  DWORD session = MAXDWORD;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return {locator, 0, 0, 0};
  // The fixture creator and client run in the same desktop session. The
  // production launcher provisions separate authority and desktop sessions.
  return {locator, creator, session, session};
}

DWORD Client(const std::wstring& name, ULONG expected_creator, ULONG expected_worker, bool lowbox) {
  if (lowbox) {
    const Handle forbidden(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX, pipe_modes,
        PIPE_UNLIMITED_INSTANCES, pipe_buffer, pipe_buffer, 0, nullptr));
    if (forbidden.get() != INVALID_HANDLE_VALUE || GetLastError() != ERROR_ACCESS_DENIED) return 20;
  }
  const User owner;
  HANDLE raw = nullptr;
  const DWORD connected = OpenProtectedDataPipeForFixture(
      Endpoint(name, expected_creator), owner.sid(), &raw);
  if (connected != ERROR_SUCCESS) return connected;
  const Handle client(raw);
  DWORD flags = 0;
  if (!GetHandleInformation(raw, &flags) || (flags & HANDLE_FLAG_INHERIT)) return 21;
  ULONG creator = 0;
  if (!GetNamedPipeServerProcessId(client.get(), &creator)) return 22;
  char byte = 'q';
  Observed served{};
  if (!Write(client.get(), byte) || !Read(client.get(), &served)) return 23;
  if (creator != expected_creator || served.opener != GetCurrentProcessId() ||
      served.worker != expected_worker || creator == served.worker) return 24;
  if (!lowbox) std::printf("observer: creator(pid=%lu) != serving child(pid=%lu); verified opener=%lu\n",
      creator, served.worker, served.opener);
  return 0;
}

void RejectImpostor(PSID owner) {
  wchar_t suffix[gesher::binder::data_pipe::kLocatorHexDigits + 1]{};
  std::swprintf(suffix, sizeof(suffix) / sizeof(suffix[0]), L"%016llx%016llx",
      static_cast<unsigned long long>(GetTickCount64()),
      static_cast<unsigned long long>(GetCurrentProcessId()));
  const std::wstring name = std::wstring(gesher::binder::data_pipe::kLocatorPrefix) + suffix;
  ProtectedDataEndpoint endpoint = Endpoint(name, GetCurrentProcessId());
  HANDLE result = nullptr;
  const auto reject = [&](DWORD expected, const char* reason) {
    const Handle server(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX |
        FILE_FLAG_FIRST_PIPE_INSTANCE, pipe_modes, 1, pipe_buffer, pipe_buffer, 0, nullptr));
    Require(server.get() != INVALID_HANDLE_VALUE, "create ordinary impostor");
    const DWORD status = OpenProtectedDataPipeForFixture(endpoint, owner, &result);
    Require(status == expected && !result, reason, status);
  };
  reject(ERROR_ACCESS_DENIED, "reject matching PID with wrong DACL");
  ++endpoint.authority_pid;
  reject(ERROR_ACCESS_DENIED, "reject wrong kernel server PID");
  --endpoint.authority_pid;
  ++endpoint.authority_session;
  reject(ERROR_ACCESS_DENIED, "reject wrong kernel server session");
  --endpoint.authority_session;
  ++endpoint.desktop_session;
  reject(ERROR_ACCESS_DENIED, "reject wrong client desktop session");
  endpoint.locator = LR"(\\attacker\pipe\gesher-private-binder-data-00000000000000000000000000000000)";
  Require(OpenProtectedDataPipeForFixture(endpoint, owner, &result) == ERROR_INVALID_PARAMETER &&
          !result, "reject remote locator before any open");

  const Handle impostor(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX |
      FILE_FLAG_FIRST_PIPE_INSTANCE, pipe_modes, 1, pipe_buffer, pipe_buffer, 0, nullptr));
  Require(impostor.get() != INVALID_HANDLE_VALUE, "create protected POSIX routing impostor");
  std::string startup_name;
  for (wchar_t ch : name) startup_name.push_back(static_cast<char>(ch));
  const auto current = Endpoint(name, GetCurrentProcessId());
  const auto pid = std::to_string(current.authority_pid);
  const auto desktop = std::to_string(current.desktop_session);
  Require(!gesher::ConfigureProtectedBinderStartup(startup_name, "-2", desktop) &&
          !gesher::ConfigureProtectedBinderStartup(startup_name, "4294967296", desktop) &&
          !gesher::ConfigureProtectedBinderStartup(startup_name, pid, "0") &&
          !gesher::ConfigureProtectedBinderStartup(startup_name, pid, "2 "),
          "reject invalid native routing and decimal values");
  Require(gesher::ConfigureProtectedBinderStartup(startup_name, pid, desktop),
          "provision the exact native routing");
  startup_name.clear();
  const auto* sealed = gesher::ProtectedBinderEndpoint();
  Require(sealed && sealed->locator == name && sealed->authority_session == 0 &&
          sealed->authority_pid == current.authority_pid &&
          sealed->desktop_session == current.desktop_session,
          "protected POSIX routing owns its startup snapshot");
  Require(gesher::ConfigureProtectedBinder({}) == ERROR_ALREADY_INITIALIZED,
          "later calls cannot switch the protected route");
  const Handle rejected(gesher::OpenProtectedBinder(*sealed));
  Require(rejected.get() == INVALID_HANDLE_VALUE, "reject ordinary pipe with exact private name");
}
DWORD Broker(HANDLE bootstrap) {
  const Handle own(bootstrap);
  for (;;) {
    HANDLE raw = nullptr;
    if (!Read(bootstrap, &raw)) return 30;
    const Handle listener(raw);
    DWORD flags = 0;
    if (!GetHandleInformation(raw, &flags) || (flags & HANDLE_FLAG_INHERIT)) return 31;
    HANDLE wider = nullptr;
    const HANDLE self = GetCurrentProcess();
    if (DuplicateHandle(self, raw, self, &wider, FILE_CREATE_PIPE_INSTANCE | WRITE_DAC,
                        FALSE, 0)) {
      CloseHandle(wider);
      return 35;
    }
    if (GetLastError() != ERROR_ACCESS_DENIED) return 36;
    if (!ConnectNamedPipe(raw, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return 32;
    char byte = 0;
    Observed proof{0, GetCurrentProcessId()};
    if (!GetNamedPipeClientProcessId(raw, &proof.opener) ||
        !Read(raw, &byte) || byte != 'q' || !Write(raw, proof)) return 33;
    if (!FlushFileBuffers(raw) || !DisconnectNamedPipe(raw)) return 34;
  }
}

void Test() {
  wchar_t image[MAX_PATH]{};
  Require(GetModuleFileNameW(nullptr, image, MAX_PATH) != 0, "get scratch executable");
  const std::wstring unique = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
  Profile container{L"gesher-binder-factory-qa-" + unique, {}};
  Profile unrelated{L"gesher-binder-outsider-qa-" + unique, {}};
  wchar_t scratch[MAX_PATH]{};
  const DWORD temporary = GetTempPathW(MAX_PATH, scratch);
  Require(temporary != 0 && temporary < MAX_PATH, "get scratch directory");
  struct ImageCopy {
    ~ImageCopy() { if (*path) DeleteFileW(path); }
    wchar_t path[MAX_PATH]{};
  } copy;
  Require(GetTempFileNameW(scratch, L"gbl", 0, copy.path), "reserve scratch executable");
  Require(CopyFileW(image, copy.path, FALSE), "copy test executable");
  PrepareProfile(copy.path, &container);
  PrepareProfile(copy.path, &unrelated);

  SECURITY_ATTRIBUTES inherited{sizeof(inherited), nullptr, TRUE};
  HANDLE reader = nullptr, writer = nullptr;
  Require(CreatePipe(&reader, &writer, &inherited, 0), "create private test bootstrap");
  Handle input(reader), output(writer);
  Require(SetHandleInformation(writer, HANDLE_FLAG_INHERIT, 0), "exclude bootstrap writer from inheritance");
  Process broker;
  Spawn(copy.path, L"--broker " + std::to_wstring(reinterpret_cast<uintptr_t>(reader)),
        &broker, container.sid.value, reader);
  input.reset();
  const User owner;
  RejectImpostor(owner.sid());
  std::unique_ptr<BinderListenerFactory> factory;
  PSID packages[] = {container.sid.value};
  const DWORD created = BinderListenerFactory::CreateForFixture(broker.info.hProcess,
      owner.sid(), packages, &factory);
  Require(created == ERROR_SUCCESS && factory, "create production-core factory", created);
  const std::wstring& path = factory->name();
  Require(gesher::binder::data_pipe::ValidLocator(path), "create protected endpoint name");

  const Handle dormant(CreateFileW(path.c_str(), client_access, 0, nullptr, OPEN_EXISTING, 0, nullptr));
  Require(dormant.get() == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY,
          "the occupied guard cannot capture a real client");

  for (DWORD i = 0; i < iterations; ++i) {
    HANDLE transferred = nullptr;
    const DWORD taken = factory->Take(&transferred);
    Require(taken == ERROR_SUCCESS && transferred, "mint listener in exact broker", taken);
    Require(Write(output.get(), transferred), "offer listener by target-local handle value");
    if (i == 1) {
      Process outsider;
      Spawn(copy.path, L"--outsider " + path + L" " + std::to_wstring(GetCurrentProcessId()),
            &outsider, unrelated.sid.value);
      outsider.Finish("deny client from non-enrolled AppContainer");

      Process app;
      Spawn(copy.path, L"--app " + path + L" " + std::to_wstring(GetCurrentProcessId()) + L" " +
          std::to_wstring(broker.info.dwProcessId), &app, container.sid.value);
      app.Finish("real AppContainer open/deny-create-instance/identity");
      std::puts("AppContainer: client open and exchange passed, CREATE_PIPE_INSTANCE denied");
    } else {
      const DWORD status = Client(path, GetCurrentProcessId(), broker.info.dwProcessId, false);
      Require(status == ERROR_SUCCESS, "host peer sees creator PID", status);
    }
  }
  HANDLE retained = nullptr;
  const DWORD taken = factory->Take(&retained);
  Require(taken == ERROR_SUCCESS && retained && Write(output.get(), retained),
          "transfer listener before revocation", taken);
  Require(factory->Revoke() == ERROR_SUCCESS, "revoke factory and retire worker");
  broker.Finish("retire exact worker with listeners", ERROR_OPERATION_ABORTED);
  retained = nullptr;
  Require(factory->Take(&retained) == ERROR_OPERATION_ABORTED && !retained, "deny reissue after revocation");
  const Handle closed(CreateFileW(path.c_str(), client_access, 0, nullptr, OPEN_EXISTING, 0, nullptr));
  Require(closed.get() == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND,
          "closing the guard and broker retires private endpoint");
  std::puts("Binder listener factory: replenishment, owner identity, AppContainer and revocation passed");
}
}
int wmain(int argc, wchar_t** argv) {
  if (argc == 4 && std::wstring_view(argv[1]) == L"--outsider") {
    const User owner;
    HANDLE raw = nullptr;
    const DWORD denied = OpenProtectedDataPipeForFixture(Endpoint(argv[2], std::wcstoul(argv[3], nullptr, 10)),
                                                         owner.sid(), &raw);
    const Handle unexpected(raw);
    return denied == ERROR_ACCESS_DENIED && !raw ? 0 : 25;
  }
  if (argc == 3 && std::wstring(argv[1]) == L"--broker")
    return static_cast<int>(Broker(reinterpret_cast<HANDLE>(std::wcstoull(argv[2], nullptr, 10))));
  if (argc == 5 && std::wstring(argv[1]) == L"--app")
    return static_cast<int>(Client(argv[2], std::wcstoul(argv[3], nullptr, 10),
        std::wcstoul(argv[4], nullptr, 10), true));
  try { Test(); return 0; }
  catch (const std::exception& e) { std::fprintf(stderr, "factory proof: %s\n", e.what()); return 1; }
}
