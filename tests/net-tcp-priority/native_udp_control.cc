// HOST fixture only: real native UDP error input to the production fd/poll path.
// The product's existing unconnected UDP reset-reporting policy is unchanged.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <mswsock.h>
#include <winternl.h>
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
void DumpOwnHandleTypes(int mode) {
  // CI-only observation, not descriptor or object identity authority. Never
  // query another process, retain snapshot handles, or ask for file/pipe names.
  using Query = NTSTATUS (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  Query process_query = nullptr, object_query = nullptr;
  HMODULE module = GetModuleHandleW(L"ntdll.dll"); require(module != nullptr, "native query module already loaded");
  FARPROC process_symbol = GetProcAddress(module, "NtQueryInformationProcess"), object_symbol = GetProcAddress(module, "NtQueryObject");
  static_assert(sizeof(process_query) == sizeof(process_symbol));
  memcpy(&process_query, &process_symbol, sizeof(process_query)); memcpy(&object_query, &object_symbol, sizeof(object_query));
  require(process_query && object_query, "actual own-process handle query APIs");
  // Native ProcessHandleInformation (51), as defined by phnt/ntpsapi.h.
  // https://github.com/winsiderss/phnt/blob/master/ntpsapi.h
  struct Header { uintptr_t count, reserved; };
  struct Entry { HANDLE handle; uintptr_t handles, pointers; ULONG access, type, attributes, reserved; };
  static_assert(sizeof(void*) == 8 && sizeof(Header) == 16 && sizeof(Entry) == 40);
  DWORD before = 0; require(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "pre-query actual handle count");
  std::vector<unsigned char> snapshot(4096); ULONG returned = 0;
  NTSTATUS status = process_query(GetCurrentProcess(), 51, snapshot.data(), static_cast<ULONG>(snapshot.size()), &returned);
  if (status == static_cast<NTSTATUS>(0xc0000004L) && returned > snapshot.size() && returned <= 1024 * 1024) {
    snapshot.resize(returned);
    status = process_query(GetCurrentProcess(), 51, snapshot.data(), static_cast<ULONG>(snapshot.size()), &returned);
  }
  printf("TCP_PRIORITY_HANDLE_SNAPSHOT mode=%d status=%lu returned=%lu before=%lu retained_aliases=0 object_names_queried=0\n", mode, static_cast<unsigned long>(status), returned, before); fflush(stdout);
  require(status >= 0 && returned >= sizeof(Header) && returned <= snapshot.size(), "bounded actual process handle snapshot");
  const auto* header = reinterpret_cast<const Header*>(snapshot.data());
  require(header->count <= (returned - sizeof(Header)) / sizeof(Entry), "actual handle entries bounded by returned bytes");
  const auto* entries = reinterpret_cast<const Entry*>(snapshot.data() + sizeof(Header));
  for (uintptr_t i = 0; i < header->count; ++i) {
    alignas(UNICODE_STRING) unsigned char type_buffer[4096]{}; ULONG type_returned = 0;
    const NTSTATUS queried = object_query(entries[i].handle, 2, type_buffer, sizeof(type_buffer), &type_returned);
    const auto* type = reinterpret_cast<const UNICODE_STRING*>(type_buffer);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(type_buffer), text = reinterpret_cast<uintptr_t>(type->Buffer);
    const bool valid = queried >= 0 && type_returned >= sizeof(UNICODE_STRING) && type_returned <= sizeof(type_buffer) &&
        type->Length % sizeof(wchar_t) == 0 && type->Length <= type->MaximumLength && text >= begin &&
        text % alignof(wchar_t) == 0 && text - begin <= type_returned && type->MaximumLength <= type_returned - (text - begin);
    printf("TCP_PRIORITY_HANDLE_TYPE mode=%d observed_value=%llu index=%lu access=%lu status=%lu type=%.*ls unretained=1\n", mode,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(entries[i].handle)), entries[i].type, entries[i].access,
        static_cast<unsigned long>(queried), valid ? static_cast<int>(type->Length / sizeof(wchar_t)) : 7, valid ? type->Buffer : L"unknown");
  }
  DWORD after = 0; require(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE, "post-query actual handle count");
  printf("TCP_PRIORITY_HANDLE_SNAPSHOT_END mode=%d entries=%llu after=%lu unchanged=%d\n", mode, static_cast<unsigned long long>(header->count), after, after == before ? 1 : 0); fflush(stdout);
  require(after == before, "diagnostic retains no additional handles");
}
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
