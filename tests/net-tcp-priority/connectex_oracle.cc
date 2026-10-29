// Independent WinSDK-only characterization. Never links Gesher.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(bool ok, const char* label) {
  if (!ok) { fprintf(stderr, "FAIL CONNECTEX %s wsa=%d win32=%lu\n", label, WSAGetLastError(), GetLastError()); exit(1); }
}
struct Target { SOCKET socket; sockaddr_in address; };
static Target target(bool listening) {
  Target t{}; t.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  require(t.socket != INVALID_SOCKET, "target socket");
  t.address.sin_family = AF_INET; t.address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(t.socket, reinterpret_cast<sockaddr*>(&t.address), sizeof(t.address)) == 0, "target bind");
  int length = sizeof(t.address);
  require(getsockname(t.socket, reinterpret_cast<sockaddr*>(&t.address), &length) == 0, "target name");
  if (listening) require(listen(t.socket, 1) == 0, "target listen");
  return t;
}
struct Operation {
  SOCKET socket = INVALID_SOCKET;
  WSAEVENT event = WSA_INVALID_EVENT;
  OVERLAPPED overlap{};
  LPFN_CONNECTEX function = nullptr;
  int start_error = -1;
  bool queued = false, completed = false;
  ULONGLONG call_ms = 0;
};
static Operation* operation(int nonblocking) {
  auto* op = new Operation;
  op->socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  require(op->socket != INVALID_SOCKET, "overlapped socket");
  op->event = WSACreateEvent(); require(op->event != WSA_INVALID_EVENT, "operation event"); op->overlap.hEvent = op->event;
  sockaddr_in local{}; local.sin_family = AF_INET;
  require(bind(op->socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "explicit wildcard bind");
  GUID id = WSAID_CONNECTEX; DWORD returned = 0;
  require(WSAIoctl(op->socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id), &op->function, sizeof(op->function), &returned, nullptr, nullptr) == 0 && returned == sizeof(op->function), "provider ConnectEx pointer");
  u_long mode = static_cast<u_long>(nonblocking);
  require(ioctlsocket(op->socket, FIONBIO, &mode) == 0, "initial native mode");
  int on = 1; DWORD timeout = 300;
  require(setsockopt(op->socket, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&on), sizeof(on)) == 0, "keepalive option");
  require(setsockopt(op->socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on)) == 0, "nodelay option");
  require(setsockopt(op->socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0, "receive option");
  return op;
}
static void submit(Operation* op, const sockaddr_in& address) {
  const ULONGLONG start = GetTickCount64();
  BOOL rc = op->function(op->socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address), nullptr, 0, nullptr, &op->overlap);
  op->start_error = rc ? 0 : WSAGetLastError(); op->call_ms = GetTickCount64() - start;
  op->queued = rc || op->start_error == ERROR_IO_PENDING;
  printf("CONNECTEX_SUBMIT rc=%d error=%d elapsed_ms=%llu queued=%d\n", rc != 0, op->start_error, op->call_ms, op->queued ? 1 : 0); fflush(stdout);
  require((op->queued || op->start_error == WSAECONNREFUSED) && op->call_ms < 1500, "actual overlapped submission is bounded");
  if (!op->queued) op->completed = true;
}
static int finish(Operation* op, const char* phase) {
  if (!op->queued) {
    printf("CONNECTEX_RESULT phase=%s immediate_error=%d no_pending_context=1\n", phase, op->start_error); fflush(stdout);
    return op->start_error;
  }
  if (op->start_error != 0) {
    const DWORD wait = WSAWaitForMultipleEvents(1, &op->event, TRUE, 5000, FALSE);
    require(wait == WSA_WAIT_EVENT_0, "owned completion event");
  }
  DWORD bytes = 0, flags = 0;
  BOOL rc = WSAGetOverlappedResult(op->socket, &op->overlap, &bytes, FALSE, &flags);
  const int error = rc ? 0 : WSAGetLastError(); op->completed = true;
  printf("CONNECTEX_RESULT phase=%s rc=%d error=%d bytes=%lu internal=%llu\n", phase, rc != 0, error, bytes, static_cast<unsigned long long>(op->overlap.Internal)); fflush(stdout);
  require(error != WSA_IO_INCOMPLETE, "event proves actual completion");
  return error;
}
static void errors(SOCKET s, const char* phase) {
  int first = -1, second = -1, length = sizeof(first);
  int a = getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&first), &length);
  length = sizeof(second); int b = getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&second), &length);
  printf("CONNECTEX_SO_ERROR phase=%s first_rc=%d first=%d second_rc=%d second=%d\n", phase, a, first, b, second); fflush(stdout);
  require(a == 0 && b == 0, "explicit caller error observations");
}
static void retire(Operation* op) {
  require(op->completed, "never free borrowed pending OVERLAPPED");
  if (op->socket != INVALID_SOCKET) require(closesocket(op->socket) == 0, "operation socket close");
  require(WSACloseEvent(op->event), "operation event close"); delete op;
}
static void data_and_options(Operation* op, Target& endpoint) {
  // Respect returned lengths: Winsock may write a one-byte Boolean rather
  // than overwriting an entire poisoned int. Zero initialization mirrors a
  // well-formed caller buffer without changing the required option values.
  int keep = 0, nodelay = 0, length = sizeof(keep); DWORD timeout = 0;
  require(getsockopt(op->socket, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<char*>(&keep), &length) == 0, "post-context keepalive");
  const int keep_length = length;
  length = sizeof(nodelay); require(getsockopt(op->socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&nodelay), &length) == 0, "post-context nodelay");
  const int nodelay_length = length;
  length = sizeof(timeout); require(getsockopt(op->socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), &length) == 0, "post-context receive timeout");
  const int timeout_length = length;
  sockaddr_in peer{}, local{}; length = sizeof(peer);
  require(getpeername(op->socket, reinterpret_cast<sockaddr*>(&peer), &length) == 0, "post-context peer");
  length = sizeof(local); require(getsockname(op->socket, reinterpret_cast<sockaddr*>(&local), &length) == 0 && local.sin_port != 0, "post-context local bind");
  printf("CONNECTEX_CONTEXT keepalive=%d keep_length=%d nodelay=%d nodelay_length=%d timeout=%lu timeout_length=%d peer_port=%u expected_port=%u local_port=%u\n", keep, keep_length, nodelay, nodelay_length, timeout, timeout_length, ntohs(peer.sin_port), ntohs(endpoint.address.sin_port), ntohs(local.sin_port)); fflush(stdout);
  require((keep_length == 1 || keep_length == static_cast<int>(sizeof(keep))) &&
          (nodelay_length == 1 || nodelay_length == static_cast<int>(sizeof(nodelay))) &&
          timeout_length == static_cast<int>(sizeof(timeout)), "actual returned option widths");
  require(keep == 1 && nodelay == 1 && timeout == 300 && peer.sin_port == endpoint.address.sin_port, "context preserves explicit properties");
  SOCKET accepted = accept(endpoint.socket, nullptr, nullptr); require(accepted != INVALID_SOCKET, "accept actual connected socket");
  require(send(accepted, "N", 1, 0) == 1, "normal payload send");
  WSAPOLLFD p{}; p.fd = op->socket; p.events = POLLRDNORM;
  require(WSAPoll(&p, 1, 2000) == 1, "normal payload wait");
  char byte = 0; require(recv(op->socket, &byte, 1, 0) == 1 && byte == 'N', "normal payload preserved");
  require(send(accepted, "!", 1, MSG_OOB) == 1, "urgent payload send");
  p.revents = 0; p.events = POLLRDBAND;
  require(WSAPoll(&p, 1, 2000) == 1 && (p.revents & POLLRDBAND), "urgent band arrived");
  require(recv(op->socket, &byte, 1, MSG_OOB) == 1 && byte == '!', "urgent payload preserved");
  p.revents = 0; require(WSAPoll(&p, 1, 0) == 0 && p.revents == 0, "urgent band cleared");
  require(closesocket(accepted) == 0, "accepted endpoint close");
}
static void scenario(bool success, int initial_mode, bool flip) {
  Target endpoint = target(success); Operation* op = operation(initial_mode);
  printf("CONNECTEX_CASE success=%d initial_mode=%d flip=%d\n", success ? 1 : 0, initial_mode, flip ? 1 : 0); fflush(stdout);
  submit(op, endpoint.address);
  if (flip) { u_long mode = initial_mode ? 0 : 1; require(ioctlsocket(op->socket, FIONBIO, &mode) == 0, "native mode change after submit"); }
  int result = finish(op, "case");
  require(result == (success ? 0 : WSAECONNREFUSED), "actual completion outcome");
  errors(op->socket, "completion-before-context");
  if (success) {
    sockaddr_in peer{}; int length = sizeof(peer);
    int before = getpeername(op->socket, reinterpret_cast<sockaddr*>(&peer), &length);
    printf("CONNECTEX_BEFORE_CONTEXT peer_rc=%d error=%d\n", before, before == SOCKET_ERROR ? WSAGetLastError() : 0);
    require(setsockopt(op->socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) == 0, "update connect context");
    data_and_options(op, endpoint);
  }
  errors(op->socket, "final-caller");
  retire(op); require(closesocket(endpoint.socket) == 0, "target close");
}
static void cancellation(bool close_socket) {
  Target endpoint = target(false); Operation* op = operation(0); submit(op, endpoint.address);
  require(op->queued && op->start_error == ERROR_IO_PENDING && !op->completed &&
          WSAWaitForMultipleEvents(1, &op->event, TRUE, 0, FALSE) == WSA_WAIT_TIMEOUT, "actual pending cancellation precondition");
  if (close_socket) {
    require(closesocket(op->socket) == 0, "close cancels outstanding operation"); op->socket = INVALID_SOCKET;
    require(WSAWaitForMultipleEvents(1, &op->event, TRUE, 5000, FALSE) == WSA_WAIT_EVENT_0, "closed socket completion retained");
    using MapStatus = ULONG(WINAPI*)(LONG);
    FARPROC symbol = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError");
    MapStatus map = nullptr; static_assert(sizeof(map) == sizeof(symbol)); memcpy(&map, &symbol, sizeof(map));
    require(map != nullptr, "completion status mapper");
    ULONG error = map(static_cast<LONG>(op->overlap.Internal));
    printf("CONNECTEX_CANCEL close=1 internal=%llu mapped=%lu\n", static_cast<unsigned long long>(op->overlap.Internal), error); fflush(stdout);
    require(error == ERROR_OPERATION_ABORTED, "last close actual canceled completion"); op->completed = true;
  } else {
    require(CancelIoEx(reinterpret_cast<HANDLE>(op->socket), &op->overlap), "explicit overlapped cancellation");
    require(finish(op, "CancelIoEx") == ERROR_OPERATION_ABORTED, "explicit actual canceled completion");
    errors(op->socket, "cancel-caller");
  }
  retire(op); require(closesocket(endpoint.socket) == 0, "cancel target close");
}
struct ThreadStart { Operation* op; sockaddr_in address; };
static DWORD WINAPI initiating_thread(void* raw) {
  auto* start = static_cast<ThreadStart*>(raw); submit(start->op, start->address); return 0;
}
static void thread_exit() {
  Target endpoint = target(false); Operation* op = operation(0); ThreadStart start{op, endpoint.address};
  HANDLE thread = CreateThread(nullptr, 0, initiating_thread, &start, 0, nullptr); require(thread != nullptr, "initiating thread create");
  require(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "initiating thread actually exited");
  DWORD code = 99; require(GetExitCodeThread(thread, &code) && code == 0 && CloseHandle(thread), "initiating thread retired");
  int result = finish(op, "initiating-thread-exited");
  printf("CONNECTEX_THREAD_EXIT completion_error=%d\n", result); fflush(stdout);
  errors(op->socket, "thread-exit-caller");
  retire(op); require(closesocket(endpoint.socket) == 0, "thread-exit target close");
}
int main() {
  WSADATA data{}; require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "startup");
  for (int success = 0; success != 2; ++success) for (int mode = 0; mode != 2; ++mode) for (int flip = 0; flip != 2; ++flip) scenario(success != 0, mode, flip != 0);
  cancellation(false); cancellation(true); thread_exit();
  require(WSACleanup() == 0, "cleanup");
  puts("PASS independent ConnectEx mode/context/error/cancel/thread-owner characterization; no production qualification");
}
