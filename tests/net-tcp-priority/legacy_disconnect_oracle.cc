// Independent provider/AF_UNSPEC characterization, never links product source.
#include <stdlib.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
using Socket = SOCKET;
static constexpr Socket invalid = INVALID_SOCKET;
static int native_error() { return WSAGetLastError(); }
static int close_socket(Socket socket) { return closesocket(socket); }
static long long millis() { return static_cast<long long>(GetTickCount64()); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
using Socket = int;
static constexpr Socket invalid = -1;
static int native_error() { return errno; }
static int close_socket(Socket socket) { return close(socket); }
static long long millis() { timespec t{}; if (clock_gettime(CLOCK_MONOTONIC, &t)) abort(); return static_cast<long long>(t.tv_sec) * 1000 + t.tv_nsec / 1000000; }
#endif
#include <stdio.h>
#include <stdlib.h>

static void require(bool value, const char* label) {
  if (!value) { fprintf(stderr, "FAIL LEGACY_DISCONNECT %s native_error=%d\n", label, native_error()); exit(1); }
}
static Socket open_socket(bool udp = false) {
#ifdef _WIN32
  Socket socket = WSASocketW(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, udp ? IPPROTO_UDP : IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  require(socket != invalid, "native socket"); u_long off = 0; require(ioctlsocket(socket, FIONBIO, &off) == 0, "actual blocking native mode");
#else
  Socket socket = ::socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, udp ? IPPROTO_UDP : IPPROTO_TCP);
  require(socket != invalid, "native socket");
#endif
  sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "native explicit bind"); return socket;
}
static void run(int state) {
  Socket target = open_socket(), client = open_socket(state == 3), accepted = invalid;
  sockaddr_in address{};
#ifdef _WIN32
  int length = sizeof(address);
  WSAPROTOCOL_INFOW provider{}; int size = sizeof(provider);
  require(getsockopt(client, SOL_SOCKET, SO_PROTOCOL_INFOW, reinterpret_cast<char*>(&provider), &size) == 0, "actual protocol provider");
  printf("LEGACY_PROVIDER state=%d family=%d type=%d protocol=%d chain=%d catalog=%lu guid=%08lx-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x flags=%08lx\n", state, provider.iAddressFamily, provider.iSocketType, provider.iProtocol, provider.ProtocolChain.ChainLen, provider.dwCatalogEntryId,
      provider.ProviderId.Data1, static_cast<unsigned>(provider.ProviderId.Data2), static_cast<unsigned>(provider.ProviderId.Data3),
      static_cast<unsigned>(provider.ProviderId.Data4[0]), static_cast<unsigned>(provider.ProviderId.Data4[1]), static_cast<unsigned>(provider.ProviderId.Data4[2]), static_cast<unsigned>(provider.ProviderId.Data4[3]),
      static_cast<unsigned>(provider.ProviderId.Data4[4]), static_cast<unsigned>(provider.ProviderId.Data4[5]), static_cast<unsigned>(provider.ProviderId.Data4[6]), static_cast<unsigned>(provider.ProviderId.Data4[7]), provider.dwServiceFlags1); fflush(stdout);
#else
  socklen_t length = sizeof(address);
#endif
  require(getsockname(target, reinterpret_cast<sockaddr*>(&address), &length) == 0, "target address");
  if (state == 2) require(listen(target, 1) == 0, "actual listening target");
  if (state != 0) {
    int result, error;
#ifdef _WIN32
    if (state == 4) {
      OVERLAPPED overlap{}; overlap.hEvent = WSACreateEvent(); require(overlap.hEvent != WSA_INVALID_EVENT, "owned diagnostic completion");
      GUID id = WSAID_CONNECTEX; LPFN_CONNECTEX connect_ex = nullptr; DWORD bytes = 0;
      require(WSAIoctl(client, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id), &connect_ex, sizeof(connect_ex), &bytes, nullptr, nullptr) == 0, "actual extension");
      BOOL start = connect_ex(client, reinterpret_cast<sockaddr*>(&address), sizeof(address), nullptr, 0, &bytes, &overlap);
      error = start ? 0 : WSAGetLastError();
      if (!start && error == ERROR_IO_PENDING) {
        require(WaitForSingleObject(overlap.hEvent, 5000) == WAIT_OBJECT_0, "actual owned refusal completion");
        DWORD flags = 0; error = WSAGetOverlappedResult(client, &overlap, &bytes, FALSE, &flags) ? 0 : WSAGetLastError();
      }
      require(error == WSAECONNREFUSED, "actual completed ConnectEx refusal");
      require(WSACloseEvent(overlap.hEvent), "diagnostic native context retired"); result = -1;
    } else
#endif
    {
      result = connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)); error = result < 0 ? native_error() : 0;
    }
    printf("LEGACY_INITIAL state=%d connect_rc=%d connect_error=%d\n", state, result, error); fflush(stdout);
#ifdef _WIN32
    require(state == 1 || state == 4 ? result == -1 && error == WSAECONNREFUSED : result == 0, "actual initial native state");
#else
    require(state == 1 || state == 4 ? result == -1 && error == ECONNREFUSED : result == 0, "actual initial native state");
#endif
    if (state == 2) { accepted = accept(target, nullptr, nullptr); require(accepted != invalid, "actual accepted transport"); }
  }
  sockaddr unspecified{}; unspecified.sa_family = AF_UNSPEC;
  printf("LEGACY_UNSPEC_BEGIN state=%d native_blocking=1\n", state); fflush(stdout);
  const long long start = millis();
  const int result = connect(client, &unspecified, sizeof(unspecified)), error = result < 0 ? native_error() : 0;
  const long long elapsed = millis() - start;
  sockaddr_in peer{}, local{}; length = sizeof(peer);
  const int peer_result = getpeername(client, reinterpret_cast<sockaddr*>(&peer), &length), peer_error = peer_result < 0 ? native_error() : 0;
  length = sizeof(local); const int local_result = getsockname(client, reinterpret_cast<sockaddr*>(&local), &length);
  int first = -1, second = -1;
#ifdef _WIN32
  length = sizeof(first); require(getsockopt(client, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&first), &length) == 0, "first actual caller error");
  length = sizeof(second); require(getsockopt(client, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&second), &length) == 0, "second actual caller error");
#else
  length = sizeof(first); require(getsockopt(client, SOL_SOCKET, SO_ERROR, &first, &length) == 0, "first actual caller error");
  length = sizeof(second); require(getsockopt(client, SOL_SOCKET, SO_ERROR, &second, &length) == 0, "second actual caller error");
#endif
  printf("LEGACY_UNSPEC_RESULT state=%d rc=%d error=%d elapsed_ms=%lld peer_rc=%d peer_error=%d local_rc=%d local_port=%u first=%d second=%d\n", state, result, error, elapsed, peer_result, peer_error, local_result, ntohs(local.sin_port), first, second); fflush(stdout);
  require(elapsed < 1500, "actual native AF_UNSPEC call bounded independently of blocking mode");
  if (accepted != invalid) require(close_socket(accepted) == 0, "accepted cleanup");
  require(close_socket(client) == 0 && close_socket(target) == 0, "native endpoints cleanup");
}
int main() {
#ifdef _WIN32
  WSADATA data{}; require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "startup");
#endif
  for (int state = 0; state != 5; ++state) run(state);
#ifdef _WIN32
  require(WSACleanup() == 0, "cleanup");
#endif
  puts("PASS independent actual legacy AF_UNSPEC/provider characterization; no product disconnect equivalence");
}
