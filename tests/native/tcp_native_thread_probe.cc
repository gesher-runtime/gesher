// Independent ordinary-Winsock observation; no Gesher product gate is replaced.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <cstdio>
#include <cstdlib>
#include "../net-tcp-priority/owner_thread_diagnostics.h"
namespace {
void Check(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "FAIL %s windows=%lu wsa=%d\n", what, GetLastError(), WSAGetLastError()); std::exit(1); }
}
void Count(int step) {
  auto snapshot = gesher::test::owner_thread_diagnostics::Capture();
  Check(snapshot.error == 0 && snapshot.close_error == 0, "complete own thread snapshot");
  DWORD handles = 0; Check(GetProcessHandleCount(GetCurrentProcess(), &handles), "own handle census");
  std::printf("NATIVE_TCP_CENSUS step=%d handles=%lu threads=%u gesher_linked=0\n", step, handles, snapshot.observed);
  std::fflush(stdout);
}
}
int main() {
  Count(-2);
  WSADATA startup{}; Check(WSAStartup(MAKEWORD(2, 2), &startup) == 0, "WSAStartup");
  Count(-1); gesher::test::ObserveOwnerThreads(0, false);
  for (int step = 0; step != 12; ++step) {
    SOCKET peer = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    SOCKET client = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    Check(peer != INVALID_SOCKET && client != INVALID_SOCKET, "native sockets");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Check(bind(peer, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "reserve closed loopback port");
    int length = sizeof(address); Check(getsockname(peer, reinterpret_cast<sockaddr*>(&address), &length) == 0, "closed peer address");
    sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Check(bind(client, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "ConnectEx local bind");
    u_long nonblocking = step % 2; Check(ioctlsocket(client, FIONBIO, &nonblocking) == 0, "initial native mode");
    GUID id = WSAID_CONNECTEX; LPFN_CONNECTEX connect_ex = nullptr; DWORD bytes = 0;
    Check(WSAIoctl(client, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id), &connect_ex, sizeof(connect_ex), &bytes, nullptr, nullptr) == 0, "ConnectEx address");
    Check(bytes == sizeof(connect_ex) && connect_ex, "ConnectEx pointer");
    OVERLAPPED overlap{}; overlap.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); Check(overlap.hEvent != nullptr, "completion event");
    BOOL issued = connect_ex(client, reinterpret_cast<sockaddr*>(&address), sizeof(address), nullptr, 0, nullptr, &overlap);
    int result = issued ? 0 : WSAGetLastError(); bool pending = result == ERROR_IO_PENDING;
    WSAPOLLFD poll{}; poll.fd = client; poll.events = POLLWRNORM;
    int before = WSAPoll(&poll, 1, 0); Check(before != SOCKET_ERROR, "native pending observation");
    nonblocking = !nonblocking; Check(ioctlsocket(client, FIONBIO, &nonblocking) == 0, "native mode flip");
    if (pending) {
      Check(WSAPoll(&poll, 1, 5000) == 1, "native refusal poll");
      Check(WaitForSingleObject(overlap.hEvent, 5000) == WAIT_OBJECT_0, "actual native completion");
      DWORD flags = 0; result = WSAGetOverlappedResult(client, &overlap, &bytes, FALSE, &flags) ? 0 : WSAGetLastError();
    }
    Check(result == WSAECONNREFUSED, "actual loopback refusal");
    Check(closesocket(client) == 0 && closesocket(peer) == 0, "both native sockets closed");
    Check(CloseHandle(overlap.hEvent), "completion event closed");
    std::printf("NATIVE_TCP_RETIRED step=%d pending=%d before=%d result=%d fixture_threads_created=0 fixture_handles_retained=0\n", step, pending, before, result);
    Count(step); gesher::test::ObserveOwnerThreads(step + 1, true);
  }
  Check(WSACleanup() == 0, "WSACleanup"); Count(12);
  std::puts("NATIVE_TCP_OBSERVATION_COMPLETE product_approval=0 original_gate_unchanged=1");
}
