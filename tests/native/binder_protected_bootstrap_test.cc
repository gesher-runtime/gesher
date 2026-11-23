// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "../../compat/binder/protected_bootstrap.h"
#include "../../compat/binder/protected_bootstrap_wire.h"

#include <cstdio>
#include <future>
#include <stdexcept>
#include <string>

namespace {

using gesher::binder::ProtectedBootstrap;
using gesher::binder::ProtectedPeers;
namespace wire = gesher::binder::bootstrap_wire;

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

std::wstring Locator() {
  wchar_t hex[wire::kLocatorHexDigits + 1];
  std::swprintf(hex, sizeof(hex) / sizeof(hex[0]), L"%016llx%016llx",
                static_cast<unsigned long long>(GetTickCount64()),
                static_cast<unsigned long long>(GetCurrentProcessId()));
  return std::wstring(wire::kPipePrefix) + hex;
}

HANDLE CreateServer(const std::wstring& locator) {
  return CreateNamedPipeW(locator.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
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
          hello.operation == wire::Operation::kHello && hello.reserved == 0, "validate hello");
    Write(server.value, wire::Request{wire::kMagic, wire::kVersion, wire::Operation::kReady, 0, 0, 0});
    ExpectReply(server.value, 0, ERROR_SUCCESS);

    // Use an actual transferred process handle. Its default ordinary ACL
    // means production registration must reject it, even inside the protocol
    // fixture; the heap-side handle must have been closed after the reply.
    HANDLE transferred = nullptr;
    Check(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
                          &transferred, 0, FALSE, DUPLICATE_SAME_ACCESS), "create transferred handle");
    Write(server.value, wire::Request{wire::kMagic, wire::kVersion, wire::Operation::kRegister,
        12345, 17, reinterpret_cast<uintptr_t>(transferred)});
    ExpectReply(server.value, 17, ERROR_ACCESS_DENIED);
    Check(GetProcessId(transferred) == 0,
          "failed registration closes its transferred handle");
    Write(server.value, wire::Request{wire::kMagic, wire::kVersion, wire::Operation::kRevoke, 0, 17, 0});
    ExpectReply(server.value, 17, ERROR_NOT_FOUND);

    if (malformed) {
      Write(server.value, wire::Request{wire::kMagic, wire::kVersion, wire::Operation::kRevoke, 0, 17, 1});
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

}  // namespace

int main() {
  try {
    UntrustedConnection();
    Transport(true);
    Transport(false);
    std::puts("Protected Binder bootstrap rejected impostor, bad frame, wrong owner, and EOF");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
