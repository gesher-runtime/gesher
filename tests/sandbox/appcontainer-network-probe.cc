// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Measures Windows TCP policy with the production AppContainer spawn path.
// Optional arguments are <public|restricted> <numeric address> <TCP port>.
// Use only test endpoints that are reachable by the ordinary Windows user.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cerrno>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "services_zygote.h"

namespace {
namespace zygote = gesher::zygote;

std::u16string U16(const std::wstring& value) {
  return {reinterpret_cast<const char16_t*>(value.data()), value.size()};
}

struct Endpoint {
  sockaddr_storage address{};
  int size = 0;
  std::wstring host;
  std::wstring port;
};

bool ParseEndpoint(const wchar_t* host, const wchar_t* port, Endpoint* endpoint) {
  wchar_t* end = nullptr;
  errno = 0;
  const unsigned long number = std::wcstoul(port, &end, 10);
  if (errno || end == port || *end || !number || number > 65535) return false;

  sockaddr_in v4{};
  sockaddr_in6 v6{};
  if (InetPtonW(AF_INET, host, &v4.sin_addr) == 1) {
    v4.sin_family = AF_INET;
    v4.sin_port = htons(static_cast<u_short>(number));
    *reinterpret_cast<sockaddr_in*>(&endpoint->address) = v4;
    endpoint->size = sizeof(v4);
  } else if (InetPtonW(AF_INET6, host, &v6.sin6_addr) == 1) {
    v6.sin6_family = AF_INET6;
    v6.sin6_port = htons(static_cast<u_short>(number));
    *reinterpret_cast<sockaddr_in6*>(&endpoint->address) = v6;
    endpoint->size = sizeof(v6);
  } else {
    return false;
  }
  endpoint->host = host;
  endpoint->port = std::to_wstring(number);
  return true;
}

int Connect(const Endpoint& endpoint) {
  SOCKET socket_handle = socket(endpoint.address.ss_family, SOCK_STREAM, IPPROTO_TCP);
  if (socket_handle == INVALID_SOCKET) return WSAGetLastError();
  u_long nonblocking = 1;
  int error = 0;
  if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) == SOCKET_ERROR) {
    error = WSAGetLastError();
  } else if (connect(socket_handle, reinterpret_cast<const sockaddr*>(&endpoint.address),
                     endpoint.size) == SOCKET_ERROR) {
    error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK) {
      fd_set write_handles;
      FD_ZERO(&write_handles);
      FD_SET(socket_handle, &write_handles);
      fd_set failed_handles = write_handles;
      timeval timeout{4, 0};
      int ready = select(0, nullptr, &write_handles, &failed_handles, &timeout);
      if (ready > 0) {
        int size = sizeof(error);
        if (getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&error), &size) == SOCKET_ERROR) {
          error = WSAGetLastError();
        }
      } else {
        error = ready == 0 ? WSAETIMEDOUT : WSAGetLastError();
      }
    }
  }
  closesocket(socket_handle);
  return error;
}

SOCKET Listen(const wchar_t* host, Endpoint* endpoint) {
  if (!ParseEndpoint(host, L"1", endpoint)) return INVALID_SOCKET;
  auto* address = reinterpret_cast<sockaddr*>(&endpoint->address);
  if (address->sa_family == AF_INET) {
    reinterpret_cast<sockaddr_in*>(address)->sin_port = 0;
  } else {
    reinterpret_cast<sockaddr_in6*>(address)->sin6_port = 0;
  }
  SOCKET listener = socket(address->sa_family, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) return listener;
  if (bind(listener, address, endpoint->size) || listen(listener, SOMAXCONN) ||
      getsockname(listener, address, &endpoint->size)) {
    closesocket(listener);
    return INVALID_SOCKET;
  }
  const u_short port = address->sa_family == AF_INET
      ? reinterpret_cast<const sockaddr_in*>(address)->sin_port
      : reinterpret_cast<const sockaddr_in6*>(address)->sin6_port;
  endpoint->port = std::to_wstring(ntohs(port));
  return listener;
}

int ContainedResult(const std::wstring& exe, const std::wstring& log,
                    const zygote::AppContainerSpec& spec, const Endpoint& endpoint) {
  auto command = U16(L"\"" + exe + L"\" --child " + endpoint.host + L" " + endpoint.port);
  int pid = zygote::SpawnInAppContainer(command, {}, {}, U16(log), spec);
  if (pid < 0) return pid;
  const int64_t start = zygote::ChildStartTime(pid);
  if (!start) return -1;
  return zygote::WaitChild(pid, start, 10000, 0, false);
}

bool Denied(int result) {
  // Windows can reject a connection explicitly or drop it without a reply.
  // Measure requires a reachable uncontained control on both sides of the test.
  return result == WSAEACCES || result == WSAETIMEDOUT;
}

bool Measure(const std::wstring& exe, const std::wstring& log, const Endpoint& endpoint,
             bool public_endpoint, zygote::AppContainerSpec spec) {
  const int direct_before = Connect(endpoint);
  const int no_network = ContainedResult(exe, log, spec, endpoint);
  spec.capabilities = {u"internetClient"};
  const int internet = ContainedResult(exe, log, spec, endpoint);
  const int direct_after = Connect(endpoint);
  std::wprintf(L"%ls [%ls]:%ls hostBefore=%d noNetwork=%d internetClient=%d hostAfter=%d\n",
               public_endpoint ? L"public" : L"restricted", endpoint.host.c_str(),
               endpoint.port.c_str(), direct_before, no_network, internet, direct_after);
  return direct_before == 0 && direct_after == 0 && Denied(no_network) &&
         (public_endpoint ? internet == 0 : Denied(internet));
}

bool IsAppContainer() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  DWORD is_contained = 0;
  DWORD read = 0;
  const bool ok = GetTokenInformation(token, TokenIsAppContainer, &is_contained,
                                      sizeof(is_contained), &read);
  CloseHandle(token);
  return ok && is_contained != 0;
}

int Usage() {
  std::fwprintf(stderr, L"usage: appcontainer-network-probe "
                        L"[<public|restricted> <numeric address> <TCP port>]...\n");
  return 2;
}

int Run(int argc, wchar_t** argv) {
  if (argc == 4 && std::wcscmp(argv[1], L"--child") == 0) {
    Endpoint endpoint;
    if (!IsAppContainer()) return 3;
    return ParseEndpoint(argv[2], argv[3], &endpoint) ? Connect(endpoint) : 2;
  }
  if ((argc - 1) % 3 != 0) return Usage();
  std::vector<std::pair<Endpoint, bool>> endpoints;
  for (int i = 1; i < argc; i += 3) {
    bool public_endpoint = std::wcscmp(argv[i], L"public") == 0;
    Endpoint endpoint;
    if ((!public_endpoint && std::wcscmp(argv[i], L"restricted") != 0) ||
        !ParseEndpoint(argv[i + 1], argv[i + 2], &endpoint)) {
      return Usage();
    }
    endpoints.emplace_back(std::move(endpoint), public_endpoint);
  }
  const std::wstring name = L"Gesher.NetworkProbe." + std::to_wstring(GetCurrentProcessId()) +
                            L"." + std::to_wstring(GetTickCount64());
  std::error_code error;
  const auto temp = std::filesystem::temp_directory_path(error);
  if (error) return 2;
  const auto root = temp / name;
  if (!std::filesystem::create_directory(root, error)) return 2;
  const auto exe = root / L"network-probe.exe";
  const auto log = root / L"probe.log";
  zygote::AppContainerSpec spec{U16(name), u"Gesher Network Probe", {}};
  std::filesystem::copy_file(std::filesystem::path(zygote::ExePath()), exe, error);
  bool passed = !error;
  if (passed) {
    unsigned status = zygote::EnsureAppContainer(spec.name, spec.display_name);
    if (!status) {
      status = zygote::GrantToAppContainer(U16(root.native()), spec.name,
                                          FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, true);
    }
    if (status) std::fwprintf(stderr, L"AppContainer setup failed: %u\n", status);
    passed = status == 0;
  }
  if (passed) {
    for (const auto* host : {L"127.0.0.1", L"::1"}) {
      Endpoint endpoint;
      SOCKET listener = Listen(host, &endpoint);
      if (listener == INVALID_SOCKET) {
        std::fwprintf(stderr, L"loopback listener unavailable: %ls\n", host);
        passed = false;
        continue;
      }
      passed = Measure(exe.native(), log.native(), endpoint, false, spec) && passed;
      closesocket(listener);
    }
    for (const auto& [endpoint, public_endpoint] : endpoints) {
      passed = Measure(exe.native(), log.native(), endpoint, public_endpoint, spec) && passed;
    }
  }
  if (zygote::DeleteAppContainer(spec.name)) passed = false;
  std::filesystem::remove(log, error);
  if (error) passed = false;
  std::filesystem::remove(exe, error);
  if (error) passed = false;
  std::filesystem::remove(root, error);
  if (error) passed = false;
  return passed ? 0 : 1;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  WSADATA data{};
  int startup = WSAStartup(MAKEWORD(2, 2), &data);
  if (startup != 0) return startup;
  int result = Run(argc, argv);
  WSACleanup();
  return result;
}
