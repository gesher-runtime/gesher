// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "../../compat/binder/protected_bootstrap.h"
#include "../../compat/binder/protected_bootstrap_wire.h"
#include "../../compat/binder/protected_data_pipe.h"

#include <cstdio>
#include <future>
#include <stdexcept>
#include <string>

namespace {

using gesher::binder::ProtectedBootstrap;
using gesher::binder::ProtectedPeers;
using gesher::binder::PeerRole;
namespace wire = gesher::binder::bootstrap_wire;
namespace data_pipe = gesher::binder::data_pipe;

void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() { Close(); }
  void Close() {
    if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
    value = INVALID_HANDLE_VALUE;
  }
  HANDLE Release() {
    const HANDLE result = value;
    value = INVALID_HANDLE_VALUE;
    return result;
  }
};

std::wstring Locator(const wchar_t* prefix = wire::kPipePrefix) {
  wchar_t hex[wire::kLocatorHexDigits + 1];
  std::swprintf(hex, sizeof(hex) / sizeof(hex[0]), L"%016llx%016llx",
                static_cast<unsigned long long>(GetTickCount64()),
                static_cast<unsigned long long>(GetCurrentProcessId()));
  return std::wstring(prefix) + hex;
}

HANDLE CreateServer(const std::wstring& locator, DWORD extra_flags = 0) {
  return CreateNamedPipeW(locator.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE |
      extra_flags,
      PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
}

template <typename T>
void Read(HANDLE pipe, T* message) {
  auto* cursor = reinterpret_cast<char*>(message);
  DWORD remaining = sizeof(T);
  while (remaining) {
    DWORD bytes = 0;
    Check(ReadFile(pipe, cursor, remaining, &bytes, nullptr) && bytes != 0, "read complete frame");
    remaining -= bytes;
    cursor += bytes;
  }
}

template <typename T>
void Write(HANDLE pipe, const T& message) {
  DWORD bytes = 0;
  Check(WriteFile(pipe, &message, sizeof(T), &bytes, nullptr) && bytes == sizeof(T),
        "write complete frame");
}

void ExpectReply(HANDLE server, uint64_t id, DWORD status) {
  wire::Reply reply{};
  Read(server, &reply);
  Check(reply.magic == wire::kMagic && reply.version == wire::kVersion &&
        reply.reserved == 0 && reply.registration_id == id && reply.status == status,
        "validate broker response");
}

void UntrustedConnection() {
  const auto locator = Locator();
  Handle pipe{CreateServer(locator)};
  Check(pipe.value != INVALID_HANDLE_VALUE, "create untrusted server pipe");
  std::unique_ptr<ProtectedBootstrap> bootstrap;
  Check(ProtectedBootstrap::Connect(L"\\\\foreign\\pipe\\bogus", &bootstrap) ==
        ERROR_INVALID_PARAMETER && !bootstrap, "reject a remote locator");
  Check(ProtectedBootstrap::Connect(locator + L"\\bogus", &bootstrap) ==
        ERROR_INVALID_PARAMETER && !bootstrap, "reject a nested locator");
  Check(ProtectedBootstrap::Connect(locator, &bootstrap) == ERROR_ACCESS_DENIED && !bootstrap,
        "reject a normal-user server impersonating the authority");
  char byte = 0;
  DWORD bytes = 0;
  Check(!ReadFile(pipe.value, &byte, 1, &bytes, nullptr) && bytes == 0,
        "no bootstrap hello was sent before authenticating the server");
  pipe.Close();
  pipe.value = CreateServer(locator);
  Check(pipe.value != INVALID_HANDLE_VALUE, "create untrusted listener bootstrap");
  Check(ProtectedBootstrap::ConnectListeners(locator, locator, &bootstrap) ==
        ERROR_INVALID_PARAMETER && !bootstrap, "reject a non-data locator");
  Check(ProtectedBootstrap::ConnectListeners(locator, Locator(data_pipe::kLocatorPrefix),
                                              &bootstrap) == ERROR_ACCESS_DENIED && !bootstrap,
        "reject a listener bootstrap impostor before Hello");
  Check(!ReadFile(pipe.value, &byte, 1, &bytes, nullptr) && bytes == 0,
        "no listener Hello before authenticating the server");
}

void Transport(bool malformed) {
  const auto locator = Locator();
  Handle server{CreateServer(locator)};
  Check(server.value != INVALID_HANDLE_VALUE, "create fixture server pipe");
  Handle client{CreateFileW(locator.c_str(), wire::kPipeClientAccess, 0, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)};
  Check(client.value != INVALID_HANDLE_VALUE, "open real fixture client pipe");
  ProtectedPeers peers;
  auto broker = std::async(std::launch::async, [pipe = client.Release(), &peers]() {
    std::unique_ptr<ProtectedBootstrap> session;
    const DWORD status = ProtectedBootstrap::AdoptFixturePipe(pipe, &session);
    return status == ERROR_SUCCESS ? session->Serve(peers) : status;
  });
  try {
    wire::Hello hello{};
    Read(server.value, &hello);
    Check(hello.magic == wire::kMagic && hello.version == wire::kVersion &&
          hello.operation == wire::Operation::kHello && hello.role == wire::Role::kRegistration,
          "validate registration Hello");
    Write(server.value, wire::Request{wire::kMagic, wire::kVersion, wire::Operation::kReady,
                                    0, 0, 0, PeerRole::kClient, 0});
    ExpectReply(server.value, 0, ERROR_SUCCESS);

    // Use an actual transferred process handle. Its default ordinary ACL
    // means production registration must reject it, even inside the protocol
    // fixture; the heap-side handle must have been closed after the reply.
    const auto reject_registration = [&](uint32_t uid, PeerRole role, DWORD expected) {
      HANDLE transferred = nullptr;
      Check(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
          &transferred, 0, FALSE, DUPLICATE_SAME_ACCESS), "create transferred handle");
      Write(server.value, wire::Request{wire::kMagic, wire::kVersion, wire::Operation::kRegister,
          uid, 17, reinterpret_cast<uintptr_t>(transferred), role, 0});
      ExpectReply(server.value, 17, expected);
      Check(GetProcessId(transferred) == 0, "failed registration closes its transferred handle");
    };
    reject_registration(12345, PeerRole::kClient, ERROR_ACCESS_DENIED);
    reject_registration(1000, PeerRole::kContextManager, ERROR_ACCESS_DENIED);
    reject_registration(12345, PeerRole::kContextManager, ERROR_INVALID_PARAMETER);
    reject_registration(1000, PeerRole::kSystemServer, ERROR_ACCESS_DENIED);
    reject_registration(12345, PeerRole::kSystemServer, ERROR_INVALID_PARAMETER);
    reject_registration(1000, static_cast<PeerRole>(42), ERROR_INVALID_PARAMETER);
    Write(server.value, wire::Request{wire::kMagic, wire::kVersion, wire::Operation::kRevoke,
                                    0, 17, 0, PeerRole::kClient, 0});
    ExpectReply(server.value, 17, ERROR_NOT_FOUND);

    if (malformed) {
      Write(server.value, wire::Request{wire::kMagic, wire::kVersion, wire::Operation::kRevoke,
                                      0, 17, 0, PeerRole::kContextManager, 0});
      Check(broker.get() == ERROR_INVALID_DATA, "malformed request closes the bootstrap");
    } else {
      server.Close();
      const DWORD status = broker.get();
      Check(status == ERROR_BROKEN_PIPE || status == ERROR_PIPE_NOT_CONNECTED,
            "bootstrap EOF fails closed");
    }
  } catch (...) {
    server.Close();
    throw;
  }
}

enum class ListenerCase {
  kSuccess, kWrongName, kOverlapped, kInheritable, kEvent, kExcessAccess, kBadFrame, kDenied, kEof,
};

void ListenerTransport(ListenerCase test) {
  const auto locator = Locator();
  const auto data_locator = Locator(data_pipe::kLocatorPrefix);
  Handle server{CreateServer(locator)};
  Check(server.value != INVALID_HANDLE_VALUE, "create listener bootstrap server");
  Handle client{CreateFileW(locator.c_str(), wire::kPipeClientAccess, 0, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)};
  Check(client.value != INVALID_HANDLE_VALUE, "open listener bootstrap client");
  auto broker = std::async(std::launch::async, [pipe = client.Release(), data_locator]() {
    std::unique_ptr<ProtectedBootstrap> session;
    const DWORD status = ProtectedBootstrap::AdoptFixtureListenerPipe(pipe, data_locator,
                                                                      &session);
    if (status != ERROR_SUCCESS) return status;
    ProtectedPeers peers;
    Check(session->Serve(peers) == ERROR_INVALID_PARAMETER, "registration role is isolated");
    HANDLE listener = nullptr;
    const DWORD take = session->TakeListener(&listener);
    if (take != ERROR_SUCCESS) {
      Check(!listener && session->TakeListener(&listener) == ERROR_INVALID_HANDLE,
            "failed delivery seals the listener channel");
      return take;
    }
    const Handle received{listener};
    Check(!ConnectNamedPipe(received.value, nullptr) && GetLastError() == ERROR_PIPE_CONNECTED,
          "adopt actual synchronous pipe listener");
    char byte = 0;
    DWORD bytes = 0;
    Check(ReadFile(received.value, &byte, 1, &bytes, nullptr) && bytes == 1 && byte == 'B',
          "use the received listener to accept actual client data");
    return take;
  });
  try {
    wire::Hello hello{};
    Read(server.value, &hello);
    Check(hello.magic == wire::kMagic && hello.version == wire::kVersion &&
          hello.operation == wire::Operation::kHello && hello.role == wire::Role::kListeners,
          "validate listener Hello");
    Write(server.value, wire::Request{wire::kMagic, wire::kVersion, wire::Operation::kReady,
                                    0, 0, 0, PeerRole::kClient, 0});
    ExpectReply(server.value, 0, ERROR_SUCCESS);

    wire::ListenerRequest request{};
    Read(server.value, &request);
    Check(request.magic == wire::kMagic && request.version == wire::kVersion &&
          request.operation == wire::Operation::kTakeListener && request.reserved == 0,
          "validate listener demand");
    if (test == ListenerCase::kEof) {
      server.Close();
      const DWORD status = broker.get();
      Check(status == ERROR_BROKEN_PIPE || status == ERROR_PIPE_NOT_CONNECTED,
            "listener bootstrap EOF fails closed");
      return;
    }
    if (test == ListenerCase::kDenied) {
      Write(server.value, wire::ListenerReply{wire::kMagic, wire::kVersion, ERROR_ACCESS_DENIED, 0, 0});
      Check(broker.get() == ERROR_ACCESS_DENIED, "authority refusal aborts the listener channel");
      return;
    }

    Handle original;
    if (test == ListenerCase::kEvent) {
      original.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(original.value != nullptr, "create impostor event");
    } else {
      auto name = data_locator;
      if (test == ListenerCase::kWrongName) name.back() = name.back() == L'a' ? L'b' : L'a';
      original.value = CreateServer(name,
          test == ListenerCase::kOverlapped ? FILE_FLAG_OVERLAPPED : 0);
      Check(original.value != INVALID_HANDLE_VALUE, "create listener capability");
    }
    Handle data_client;
    if (test == ListenerCase::kSuccess) {
      data_client.value = CreateFileW(data_locator.c_str(), data_pipe::kClientAccess, 0, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
      Check(data_client.value != INVALID_HANDLE_VALUE, "connect a real guest data pipe");
      Write(data_client.value, 'B');
    }
    HANDLE duplicate = nullptr;
    constexpr DWORD listener_access =
        FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE;
    Check(DuplicateHandle(GetCurrentProcess(), original.value, GetCurrentProcess(), &duplicate,
                          listener_access, test == ListenerCase::kInheritable,
                          test == ListenerCase::kEvent || test == ListenerCase::kExcessAccess
                              ? DUPLICATE_SAME_ACCESS : 0),
          "duplicate capability with factory rights");
    original.Close();
    Write(server.value, wire::ListenerReply{wire::kMagic, wire::kVersion, ERROR_SUCCESS,
        test == ListenerCase::kBadFrame ? 1u : 0u, reinterpret_cast<uintptr_t>(duplicate)});
    const DWORD status = broker.get();
    Check(test == ListenerCase::kSuccess ? status == ERROR_SUCCESS : status != ERROR_SUCCESS,
          "accept only the exact expected listener");
    DWORD flags = 0;
    Check(!GetHandleInformation(duplicate, &flags), "the receiver closed its transferred handle");
  } catch (...) {
    server.Close();
    throw;
  }
}

}  // namespace

int main() {
  try {
    UntrustedConnection();
    Transport(true);
    Transport(false);
    for (const auto test : {ListenerCase::kSuccess, ListenerCase::kWrongName,
                            ListenerCase::kOverlapped, ListenerCase::kInheritable,
                            ListenerCase::kEvent, ListenerCase::kExcessAccess,
                            ListenerCase::kBadFrame, ListenerCase::kDenied, ListenerCase::kEof}) {
      ListenerTransport(test);
    }
    std::puts("Protected Binder bootstrap validated registration and private listener channels");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
