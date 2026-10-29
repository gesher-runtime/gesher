// HOST fixture only: real native UDP error input to the production fd/poll path.
// The product's existing unconnected UDP reset-reporting policy is unchanged.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../compat/posix/src/socket_internal.h"

namespace {
void require(bool value, const char* label) {
  if (!value) { fprintf(stderr, "FAIL TCP native UDP control: %s wsa=%d\n", label, WSAGetLastError()); exit(1); }
}
template<class Fn> Fn native(const char* name) {
  FARPROC symbol = GetProcAddress(GetModuleHandleW(L"ws2_32.dll"), name);
  Fn function = nullptr; static_assert(sizeof(function) == sizeof(symbol)); memcpy(&function, &symbol, sizeof(function));
  require(function != nullptr, "canonical native entry"); return function;
}
}
namespace gesher::test {
int NativeUdpErrorControl() {
  require(ws::Startup(), "Winsock startup");
  auto bind_native = native<decltype(&::bind)>("bind");
  auto name_native = native<decltype(&::getsockname)>("getsockname");
  auto connect_native = native<decltype(&::connect)>("connect");
  auto send_native = native<decltype(&::send)>("send");
  SOCKET target = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  SOCKET client = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  require(target != INVALID_SOCKET && client != INVALID_SOCKET, "owned actual native endpoints");
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind_native(target, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "target bind");
  int length = sizeof(address); require(name_native(target, reinterpret_cast<sockaddr*>(&address), &length) == 0, "target address");
  sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind_native(client, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "client bind");
  BOOL report = TRUE; DWORD bytes = 0;
  require(WSAIoctl(client, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &bytes, nullptr, nullptr) == 0, "positive native error reporting input");
  u_long mode = 1; require(ioctlsocket(client, FIONBIO, &mode) == 0, "actual native nonblocking input");
  require(connect_native(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && closesocket(target) == 0, "real closed peer port");
  require(send_native(client, "u", 1, 0) == 1, "real loopback ICMP-producing datagram");
  WSAPOLLFD p{}; p.fd = client; p.events = POLLRDNORM | POLLRDBAND;
  const int result = WSAPoll(&p, 1, 2000);
  const int native_error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
  printf("TCP_PRIORITY_NATIVE_UDP_INPUT poll=%d revents=%d native_error=%d reset_reporting=1 payload_or_SO_ERROR_consumed=0\n", result, p.revents, native_error); fflush(stdout);
  require(result == 1 && (p.revents & POLLERR), "positive native UDP error really exists before mapping");
  SocketFd state; state.socket = static_cast<ws::Socket>(client); state.initial_nonblocking = true;
  const int fd = RegisterSocket(state); require(fd >= 0, "production fd owns native error endpoint");
  return fd;
}
}
