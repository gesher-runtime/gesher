// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Real Windows process, file-object and mapping tests. The isolated registry
// substitutes the fixture account only for LocalSystem object ownership; it
// does not install a service, open disks or advertise a production device.
#include "../../compat/binder/protected_shared_memory.h"

#include <sddl.h>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using gesher::binder::PeerRole;
using gesher::binder::ProtectedPeers;
using gesher::binder::ProtectedSharedMemory;

constexpr uint64_t kSize = 4096;
constexpr DWORD kWait = 3000;

void Require(bool value, const char* operation) {
  if (!value) throw std::runtime_error(std::string(operation) +
      " failed, Windows error " + std::to_string(GetLastError()));
}

struct HandleOwner {
  void operator()(void* value) const {
    if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
  }
};
using Handle = std::unique_ptr<void, HandleOwner>;

struct Process {
  PROCESS_INFORMATION value{};
  ~Process() {
    if (!value.hProcess) return;
    TerminateProcess(value.hProcess, 0);
    WaitForSingleObject(value.hProcess, kWait);
    CloseHandle(value.hThread);
    CloseHandle(value.hProcess);
  }
};

struct File {
  std::wstring path;
  Handle handle;
  File() {
    wchar_t folder[MAX_PATH], name[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, folder);
    Require(length && length < MAX_PATH && GetTempFileNameW(folder, L"asm", 0, name),
            "create isolated memory fixture path");
    path = name;
    handle.reset(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    Require(handle.get() != INVALID_HANDLE_VALUE, "open isolated memory fixture");
    LARGE_INTEGER size{};
    size.QuadPart = kSize;
    Require(SetFilePointerEx(handle.get(), size, nullptr, FILE_BEGIN) &&
        SetEndOfFile(handle.get()), "size real shared memory backing file");
  }
  void Write(uint32_t value) {
    DWORD written = 0;
    Require(SetFilePointer(handle.get(), 0, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
        WriteFile(handle.get(), &value, sizeof(value), &written, nullptr) && written == sizeof(value),
        "update actual producer memory");
  }
};

void Start(PSID owner, Process* process) {
  wchar_t* sid_text = nullptr;
  Require(ConvertSidToStringSidW(owner, &sid_text), "format fixture owner");
  const std::wstring descriptor = L"O:" + std::wstring(sid_text) +
      L"D:P(A;;0x001fffff;;;" + sid_text + L")";
  LocalFree(sid_text);
  PSECURITY_DESCRIPTOR security = nullptr;
  Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(),
      SDDL_REVISION_1, &security, nullptr), "create exact fixture process DACL");
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), security, FALSE};
  wchar_t image[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, image, MAX_PATH);
  Require(length && length < MAX_PATH, "resolve fixture image");
  std::wstring command = L"\"" + std::wstring(image, length) + L"\" --idle";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  const BOOL launched = CreateProcessW(image, command.data(), &attributes, nullptr, FALSE,
      CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process->value);
  LocalFree(security);
  Require(launched, "create actual suspended process");
}

uint64_t Put(HANDLE process, HANDLE object) {
  HANDLE remote = nullptr;
  Require(DuplicateHandle(GetCurrentProcess(), object, process, &remote, 0, FALSE,
                          DUPLICATE_SAME_ACCESS), "place exact object in producer");
  return reinterpret_cast<uintptr_t>(remote);
}

Handle Take(HANDLE process, uint64_t remote) {
  HANDLE local = nullptr;
  Require(DuplicateHandle(process, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(remote)),
      GetCurrentProcess(), &local, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE),
      "recover exact consumer access for kernel validation");
  return Handle(local);
}

void Run() {
  HANDLE raw_token = nullptr;
  Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token), "open fixture identity");
  const Handle token(raw_token);
  alignas(TOKEN_USER) std::byte user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  DWORD bytes = 0;
  Require(GetTokenInformation(token.get(), TokenUser, user, sizeof(user), &bytes),
          "read real fixture identity");
  PSID owner = reinterpret_cast<TOKEN_USER*>(user)->User.Sid;

  Process producer, consumer, impostor, second_server, manager;
  Start(owner, &producer);
  Start(owner, &consumer);
  Start(owner, &impostor);
  Start(owner, &second_server);
  Start(owner, &manager);
  ProtectedPeers peers;
  ProtectedPeers::Peer system, app, forged, other, context;
  Require(peers.RegisterForFixture(producer.value.hProcess, 1000, PeerRole::kSystemServer,
      owner, &system) == ERROR_SUCCESS, "register exact system server creation object");
  Require(peers.RegisterForFixture(consumer.value.hProcess, 10042, PeerRole::kClient,
      owner, &app) == ERROR_SUCCESS, "register exact app creation object");
  Require(peers.RegisterForFixture(impostor.value.hProcess, 1000, PeerRole::kClient,
      owner, &forged) == ERROR_SUCCESS, "register same-UID process without producer role");
  Require(peers.RegisterForFixture(second_server.value.hProcess, 1000, PeerRole::kSystemServer,
      owner, &other) == ERROR_SUCCESS, "register separately admitted server epoch");
  Require(peers.RegisterForFixture(manager.value.hProcess, 1000, PeerRole::kContextManager,
      owner, &context) == ERROR_SUCCESS, "register native context manager");

  File memory, unrelated;
  memory.Write(0x3ca517e1);
  const uint64_t source = Put(system->process(), memory.handle.get());
  const uint64_t forged_source = Put(forged->process(), memory.handle.get());
  const uint64_t other_source = Put(other->process(), memory.handle.get());
  const uint64_t substitute = Put(system->process(), unrelated.handle.get());
  ProtectedSharedMemory region;
  uint64_t remote = 0;
  Require(region.Duplicate(system, app, source, &remote) == ERROR_ACCESS_DENIED && !remote,
          "reject transfer before publication");
  Require(region.Register(forged, forged_source, kSize) == ERROR_ACCESS_DENIED,
          "same UID cannot impersonate producer role");
  Require(region.Register(system, source, kSize + 1) == ERROR_INVALID_DATA,
          "reject a fabricated region size");
  Require(region.Register(system, source, kSize) == ERROR_SUCCESS,
          "publish actual producer object");
  Require(region.Register(system, source, kSize) == ERROR_SUCCESS,
          "same-object publication is idempotent");
  Require(region.Register(system, substitute, kSize) == ERROR_ALREADY_EXISTS,
          "reject producer substitution after publication");
  Require(region.Register(other, other_source, kSize) == ERROR_ALREADY_EXISTS,
          "reject a different admitted producer of the same object");
  Require(region.Duplicate(other, app, other_source, &remote) == ERROR_ACCESS_DENIED && !remote,
          "bind resource to exact original producer process");
  Require(region.Duplicate(forged, app, forged_source, &remote) == ERROR_ACCESS_DENIED && !remote,
          "reject producer impersonation during transfer");
  Require(region.Duplicate(system, app, substitute, &remote) == ERROR_ACCESS_DENIED && !remote,
          "reject unrelated file from the real producer");

  const Handle reopened(CreateFileW(memory.path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
  Require(reopened.get() != INVALID_HANDLE_VALUE, "reopen same file as a distinct kernel object");
  const uint64_t reopened_source = Put(system->process(), reopened.get());
  Require(region.Duplicate(system, app, reopened_source, &remote) == ERROR_ACCESS_DENIED && !remote,
          "reject equal pathname/file ID with a different open object");
  const Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  Require(event != nullptr, "create control handle rejection fixture");
  const uint64_t event_source = Put(system->process(), event.get());
  Require(region.Duplicate(system, app, event_source, &remote) != ERROR_SUCCESS && !remote,
          "reject event/control handle transfer");
  const uint64_t process_source = Put(system->process(), producer.value.hProcess);
  Require(region.Duplicate(system, app, process_source, &remote) != ERROR_SUCCESS && !remote,
          "reject process control handle transfer");
  Require(region.Duplicate(system, context, source, &remote) == ERROR_ACCESS_DENIED && !remote,
          "context manager is not an application consumer");

  Require(region.Duplicate(system, app, source, &remote) == ERROR_SUCCESS && remote,
          "transfer only the registered actual object");
  const Handle received = Take(app->process(), remote);
  remote = 0;
  DWORD flags = 0;
  Require(GetHandleInformation(received.get(), &flags) && !(flags & HANDLE_FLAG_INHERIT),
          "consumer handle is noninheritable");
  uint32_t value = 0;
  DWORD read = 0;
  Require(SetFilePointer(received.get(), 0, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
      ReadFile(received.get(), &value, sizeof(value), &read, nullptr) &&
      read == sizeof(value) && value == 0x3ca517e1, "consumer reads actual producer bytes");
  Require(!WriteFile(received.get(), &value, sizeof(value), &read, nullptr) &&
      GetLastError() == ERROR_ACCESS_DENIED, "kernel denies writing the consumer handle");
  Require(!CreateFileMappingW(received.get(), nullptr, PAGE_READWRITE, 0, 0, nullptr) &&
      GetLastError() == ERROR_ACCESS_DENIED, "kernel denies a writable section");
  const Handle mapping(CreateFileMappingW(received.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
  Require(mapping != nullptr, "create real readonly shared mapping");
  Require(!MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, 0), "deny writable consumer mapping");
  void* view = MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, 0);
  Require(view != nullptr, "map readonly application shared memory");
  DWORD old = 0;
  Require(!VirtualProtect(view, kSize, PAGE_READWRITE, &old), "deny later write protection upgrade");
  memory.Write(0x72bed430);
  Require(*static_cast<const volatile uint32_t*>(view) == 0x72bed430,
          "consumer sees live producer changes, not a fabricated snapshot");
  Require(UnmapViewOfFile(view), "unmap real consumer view");

  peers.Revoke(app);
  Require(region.Duplicate(system, app, source, &remote) == ERROR_ACCESS_DENIED && !remote,
          "revoke existing consumer before any new transfer");
  peers.Revoke(system);
  Require(region.Register(system, source, kSize) == ERROR_ACCESS_DENIED,
          "revoked producer cannot republish");
  Require(region.Duplicate(system, forged, source, &remote) == ERROR_ACCESS_DENIED && !remote,
          "revoked producer cannot transfer");
  Require(TerminateProcess(other->process(), 0) == FALSE,
          "registry process handle does not itself grant termination");
  Require(TerminateProcess(second_server.value.hProcess, 0) &&
      WaitForSingleObject(second_server.value.hProcess, kWait) == WAIT_OBJECT_0,
      "retire an admitted exact process");
  Require(region.Register(other, other_source, kSize) == ERROR_ACCESS_DENIED,
          "exited producer cannot publish using a stale process epoch");
}
}  // namespace

int wmain(int argc, wchar_t**) {
  if (argc != 1) return 0;  // fixture children remain suspended until retired
  try {
    Run();
    std::puts("protected ApplicationSharedMemory: exact producer/object, readonly mappings, revocation PASS");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "protected ApplicationSharedMemory: %s\n", error.what());
    return 1;
  }
}
