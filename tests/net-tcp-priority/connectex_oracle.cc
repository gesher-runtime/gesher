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
  sockaddr_in address{};  // owned through completion, never a caller-stack borrow
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
  op->address = address;
  const ULONGLONG start = GetTickCount64();
  BOOL rc = op->function(op->socket, reinterpret_cast<const sockaddr*>(&op->address), sizeof(op->address), nullptr, 0, nullptr, &op->overlap);
  op->start_error = rc ? 0 : WSAGetLastError(); op->call_ms = GetTickCount64() - start;
  op->queued = rc || op->start_error == ERROR_IO_PENDING;
  printf("CONNECTEX_SUBMIT rc=%d error=%d elapsed_ms=%llu queued=%d\n", rc != 0, op->start_error, op->call_ms, op->queued ? 1 : 0); fflush(stdout);
  require((op->queued || op->start_error == WSAECONNREFUSED) && op->call_ms < 1500, "actual overlapped submission is bounded");
  if (!op->queued) op->completed = true;
}
static void poll_state(Operation* op, const char* phase) {
  WSAPOLLFD band{}; band.fd = op->socket; band.events = POLLRDBAND;
  int band_rc = WSAPoll(&band, 1, 0); int band_error = band_rc == SOCKET_ERROR ? WSAGetLastError() : 0;
  WSAPOLLFD all{}; all.fd = op->socket; all.events = POLLRDBAND | POLLRDNORM | POLLWRNORM;
  int all_rc = WSAPoll(&all, 1, 0); int all_error = all_rc == SOCKET_ERROR ? WSAGetLastError() : 0;
  fd_set exceptions; FD_ZERO(&exceptions); FD_SET(op->socket, &exceptions); timeval zero{};
  int except_rc = select(0, nullptr, nullptr, &exceptions, &zero);
  int except_error = except_rc == SOCKET_ERROR ? WSAGetLastError() : 0;
  // A provider's negative readiness result is itself an observation, not
  // permission to discard the independently owned completion status.
  printf("CONNECTEX_POLL phase=%s band_rc=%d band=%d band_error=%d all_rc=%d all=%d all_error=%d except_rc=%d except_error=%d internal=%llu SO_ERROR=not-read\n",
         phase, band_rc, band.revents, band_error, all_rc, all.revents, all_error, except_rc, except_error, static_cast<unsigned long long>(op->overlap.Internal)); fflush(stdout);
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
  poll_state(op, "event-before-query");
  DWORD bytes = 0, flags = 0;
  BOOL rc = WSAGetOverlappedResult(op->socket, &op->overlap, &bytes, FALSE, &flags);
  const int error = rc ? 0 : WSAGetLastError(); op->completed = true;
  printf("CONNECTEX_RESULT phase=%s rc=%d error=%d bytes=%lu internal=%llu\n", phase, rc != 0, error, bytes, static_cast<unsigned long long>(op->overlap.Internal)); fflush(stdout);
  require(error != WSA_IO_INCOMPLETE, "event proves actual completion");
  poll_state(op, "after-owned-query");
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
struct Owner {
  CRITICAL_SECTION mutex;
  HANDLE wake = nullptr, accepted = nullptr, thread = nullptr;
  Operation* queued = nullptr;
  sockaddr_in address{};
  bool stop = false;
};
static DWORD WINAPI owner_thread(void* raw) {
  auto* owner = static_cast<Owner*>(raw);
  for (;;) {
    require(WaitForSingleObject(owner->wake, 10000) == WAIT_OBJECT_0, "owner bounded wake");
    EnterCriticalSection(&owner->mutex);
    const bool stop = owner->stop; Operation* op = owner->queued; sockaddr_in address = owner->address;
    owner->queued = nullptr; LeaveCriticalSection(&owner->mutex);
    if (stop) return 0;
    require(op != nullptr, "owner exact queued operation");
    submit(op, address);
    printf("CONNECTEX_OWNER_SUBMIT thread=%lu\n", GetCurrentThreadId()); fflush(stdout);
    require(SetEvent(owner->accepted), "owner publishes submitted operation");
  }
}
struct Request { Owner* owner; Operation* op; sockaddr_in address; };
static DWORD WINAPI request_thread(void* raw) {
  auto* request = static_cast<Request*>(raw); auto* owner = request->owner;
  EnterCriticalSection(&owner->mutex);
  require(owner->queued == nullptr && !owner->stop, "request owns empty queue slot");
  owner->queued = request->op; owner->address = request->address;
  LeaveCriticalSection(&owner->mutex);
  require(SetEvent(owner->wake) && WaitForSingleObject(owner->accepted, 5000) == WAIT_OBJECT_0, "request actual owner submission ACK");
  return 0;  // caller exits; it never initiated the native IO
}
static void persistent_owner() {
  Owner owner; InitializeCriticalSection(&owner.mutex);
  owner.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr); owner.accepted = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(owner.wake != nullptr && owner.accepted != nullptr, "persistent owner events");
  owner.thread = CreateThread(nullptr, 0, owner_thread, &owner, 0, nullptr); require(owner.thread != nullptr, "persistent owner thread");
  for (int success = 0; success != 2; ++success) {
    Target endpoint = target(success != 0); Operation* op = operation(success);
    Request request{&owner, op, endpoint.address};
    HANDLE caller = CreateThread(nullptr, 0, request_thread, &request, 0, nullptr); require(caller != nullptr, "requesting caller thread");
    require(WaitForSingleObject(caller, 5000) == WAIT_OBJECT_0, "requester actually exited");
    DWORD code = 99; require(GetExitCodeThread(caller, &code) && code == 0 && CloseHandle(caller), "requester exact retirement");
    require(WaitForSingleObject(owner.thread, 0) == WAIT_TIMEOUT, "IO owner remains live after requester exit");
    if (!success) require(op->queued && WSAWaitForMultipleEvents(1, &op->event, TRUE, 0, FALSE) == WSA_WAIT_TIMEOUT, "requester exited while IO was still pending");
    poll_state(op, "requester-exited-owner-live");
    const int result = finish(op, "persistent-owner");
    require(result == (success ? 0 : WSAECONNREFUSED), "live owner preserves actual IO outcome");
    require(WaitForSingleObject(owner.thread, 0) == WAIT_TIMEOUT, "same owner retained through completion");
    printf("CONNECTEX_OWNER caller_exited=1 worker_live=1 success=%d completion=%d owner_thread=%lu\n", success, result, GetThreadId(owner.thread)); fflush(stdout);
    errors(op->socket, "persistent-owner-caller");
    poll_state(op, "persistent-owner-after-caller");
    if (success) {
      require(setsockopt(op->socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) == 0, "owner success context update");
      data_and_options(op, endpoint);
    }
    retire(op); require(closesocket(endpoint.socket) == 0, "owner target retired");
  }
  // The positive owner exits only after every request context has completed
  // and been retired. Shutdown with outstanding IO is not silently accepted.
  EnterCriticalSection(&owner.mutex); require(owner.queued == nullptr, "owner queue drained before stop"); owner.stop = true; LeaveCriticalSection(&owner.mutex);
  require(SetEvent(owner.wake) && WaitForSingleObject(owner.thread, 5000) == WAIT_OBJECT_0, "persistent owner stops after drain");
  DWORD code = 99; require(GetExitCodeThread(owner.thread, &code) && code == 0, "owner normal exit");
  require(CloseHandle(owner.thread) && CloseHandle(owner.wake) && CloseHandle(owner.accepted), "owner handles retired");
  DeleteCriticalSection(&owner.mutex);
  puts("PASS independent persistent IO owner: requester exit, two native outcomes, completion drain and bounded shutdown");
}
int main() {
  WSADATA data{}; require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "startup");
  for (int success = 0; success != 2; ++success) for (int mode = 0; mode != 2; ++mode) for (int flip = 0; flip != 2; ++flip) scenario(success != 0, mode, flip != 0);
  cancellation(false); cancellation(true); thread_exit();
  persistent_owner();
  require(WSACleanup() == 0, "cleanup");
  puts("PASS independent ConnectEx mode/context/error/cancel/thread-owner characterization; no production qualification");
}
