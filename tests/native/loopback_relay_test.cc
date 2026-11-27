// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <winsock2.h>
#include <ws2tcpip.h>
#include "../../compat/binder/loopback_relay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace relay = gesher::loopback_relay;

namespace {

void Require(bool passed, const char* assertion) {
  if (!passed) {
    std::fprintf(stderr, "FAIL: %s (Win32 %lu, WSA %d)\n", assertion, GetLastError(), WSAGetLastError());
    std::exit(1);
  }
}

class Lane {
 public:
  explicit Lane(relay::Request request, bool authorized = true, std::function<void()> wake = {}) {
    static unsigned sequence = 0;
    name_ = L"\\\\.\\pipe\\GesherLoopbackRelayTest-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(++sequence);
    pipe_ = CreateNamedPipeW(name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, nullptr);
    Require(pipe_ != INVALID_HANDLE_VALUE, "create owned relay pipe");
    live_.store(authorized);
    worker_ = std::thread([this, request] {
      const BOOL connected = ConnectNamedPipe(pipe_, nullptr);
      if (connected || GetLastError() == ERROR_PIPE_CONNECTED)
        relay::Serve(pipe_, request, [this] { return live_.load(); });
      DisconnectNamedPipe(pipe_);
      CloseHandle(pipe_);
    });
    client = relay::Client::Start(request,
        [name = name_](const relay::Request&) {
          return CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
              OPEN_EXISTING, 0, nullptr);
        }, [counter = notified_, wake = std::move(wake)] {
          counter->fetch_add(1);
          if (wake) wake();
        }, nullptr);
  }
  ~Lane() {
    client->Close();
    if (worker_.joinable()) worker_.join();
    client.reset();
  }
  relay::Snapshot Connected() {
    const auto deadline = GetTickCount64() + 7000;
    auto state = client->State();
    while (state.connecting && GetTickCount64() < deadline) {
      client->WaitForChange(state.revision, 100);
      state = client->State();
    }
    Require(!state.connecting, "bounded connect completion");
    return state;
  }
  void Revoke() { live_.store(false); }
  std::shared_ptr<relay::Client> client;

 private:
  std::wstring name_;
  HANDLE pipe_;
  std::thread worker_;
  std::atomic<bool> live_{false};
  std::shared_ptr<std::atomic<unsigned>> notified_ = std::make_shared<std::atomic<unsigned>>(0);
};

SOCKET Listener(uint16_t* port) {
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  Require(listener != INVALID_SOCKET, "real listener socket");
  sockaddr_in ip{};
  ip.sin_family = AF_INET;
  ip.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  Require(bind(listener, reinterpret_cast<sockaddr*>(&ip), sizeof(ip)) == 0 &&
      listen(listener, 1) == 0, "listen on owned loopback endpoint");
  int size = sizeof(ip);
  Require(getsockname(listener, reinterpret_cast<sockaddr*>(&ip), &size) == 0,
      "read exact listener address");
  *port = ntohs(ip.sin_port);
  return listener;
}

relay::Request Target(uint16_t port) {
  relay::Request request;
  request.target.family = 4;
  request.target.address[0] = 127;
  request.target.address[3] = 1;
  request.target.port = port;
  return request;
}

void EarlySource() {
  uint16_t port = 0;
  SOCKET listener = Listener(&port);
  HANDLE source = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE allow_connect = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Require(source && allow_connect, "create early-source witness events");
  auto first = std::make_shared<std::atomic<bool>>(true);
  {
    Lane lane(Target(port), true, [=] {
      if (first->exchange(false)) {
        SetEvent(source);
        WaitForSingleObject(allow_connect, 2000);
      }
    });
    Require(WaitForSingleObject(source, 2000) == WAIT_OBJECT_0, "source bound notification arrived");
    const auto early = lane.client->State();
    Require(early.connecting && !early.connected && !early.error &&
            relay::IsLoopback(early.local) && !early.peer.family,
            "separate actual bound source while still connecting");
    fd_set reads;
    FD_ZERO(&reads);
    FD_SET(listener, &reads);
    timeval immediate{};
    Require(select(0, &reads, nullptr, nullptr, &immediate) == 0,
            "target has no connection before source acknowledgement");
    SetEvent(allow_connect);
    const auto final = lane.Connected();
    Require(final.connected && final.local.port == early.local.port && final.peer.port == port,
            "actual completion preserves the early source");
    sockaddr_in real_source{};
    int size = sizeof(real_source);
    SOCKET peer = accept(listener, reinterpret_cast<sockaddr*>(&real_source), &size);
    Require(peer != INVALID_SOCKET && ntohs(real_source.sin_port) == early.local.port,
            "real target confirms the early source port");
    closesocket(peer);
  }
  CloseHandle(source);
  CloseHandle(allow_connect);
  closesocket(listener);
  std::puts("PASS real bound source before actual connect, observed by the TCP target");
}

void ForkAdmission() {
  for (int i = 0; i < 700 && relay::HasProcessLocalState(); ++i) Sleep(10);
  Require(relay::TryLockForkGate(), "quiescent relay fork admission");
  HANDLE entering = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE published = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Require(entering && published && release, "create fork witness events");
  std::thread concurrent([&] {
    SetEvent(entering);
    auto client = relay::Client::Start({}, [](const relay::Request&) { return INVALID_HANDLE_VALUE; }, {}, nullptr);
    SetEvent(published);
    WaitForSingleObject(release, 4000);
  });
  Require(WaitForSingleObject(entering, 2000) == WAIT_OBJECT_0, "opener reached fork barrier");
  Require(WaitForSingleObject(published, 100) == WAIT_TIMEOUT && !relay::HasProcessLocalState(),
      "creation is blocked while the fork gate is held");
  relay::UnlockForkGate();
  Require(WaitForSingleObject(published, 2000) == WAIT_OBJECT_0, "creation resumes after the fork gate");
  for (int i = 0; i < 200 && relay::HasActiveWorkers(); ++i) Sleep(10);
  Require(!relay::HasActiveWorkers() && relay::HasProcessLocalState() && !relay::TryLockForkGate(),
      "failed client object with no worker still holds process-local state");
  SetEvent(release);
  concurrent.join();
  Require(relay::TryLockForkGate(), "releasing the last client restores fork admission");
  relay::UnlockForkGate();
  CloseHandle(entering); CloseHandle(published); CloseHandle(release);
  std::puts("PASS simultaneous creation is excluded by the fork gate and closed clients still block");
}

void Stream() {
  uint16_t port = 0;
  SOCKET listener = Listener(&port);
  auto request = Target(port);
  constexpr size_t kBytes = 180000;
  const auto pattern = [](size_t offset) { return static_cast<std::byte>(offset % 253); };
  std::thread target([&] {
    SOCKET peer = accept(listener, nullptr, nullptr);
    Require(peer != INVALID_SOCKET, "accept real broker connection");
    size_t total = 0;
    char bytes[4096];
    while (true) {
      const int n = recv(peer, bytes, sizeof(bytes), 0);
      Require(n >= 0, "real target recv");
      if (n == 0) break;
      for (int i = 0; i < n; ++i)
        Require(static_cast<std::byte>(bytes[i]) == pattern(total++), "ordered exact bytes");
    }
    Require(total == kBytes, "real target saw FIN after all bytes");
    size_t exported = 0;
    while (exported < kBytes) {
      const size_t size = (std::min)(sizeof(bytes), kBytes - exported);
      for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<char>(pattern(exported + i));
      const int n = send(peer, bytes, static_cast<int>(size), 0);
      Require(n > 0, "target reply bytes");
      exported += static_cast<size_t>(n);
    }
    Require(shutdown(peer, SD_SEND) == 0, "target half close");
    closesocket(peer);
  });
  Lane lane(request);
  const auto ready = lane.Connected();
  Require(relay::HasActiveWorkers(), "active relay prevents a process-state snapshot");
  Require(ready.connected && ready.error == 0 && ready.peer.port == port &&
      ready.local.port != 0 && relay::IsLoopback(ready.local), "actual connected address metadata");
  const auto deadline = GetTickCount64() + 9000;
  size_t sent = 0;
  std::array<std::byte, 913> bytes{};
  while (sent < kBytes) {
    Require(GetTickCount64() < deadline, "bounded transfer under backpressure");
    const auto state = lane.client->State();
    const size_t size = (std::min)(bytes.size(), kBytes - sent);
    for (size_t i = 0; i < size; ++i) bytes[i] = pattern(sent + i);
    size_t count = 0;
    const int result = lane.client->Send({bytes.data(), size}, &count);
    Require(result == 0 || result == WSAEWOULDBLOCK, "relay send");
    sent += count;
    if (result == WSAEWOULDBLOCK) lane.client->WaitForChange(state.revision, 100);
  }
  Require(lane.client->Shutdown(false, true) == 0, "send half-close");
  size_t received = 0;
  const auto queued_deadline = GetTickCount64() + 4000;
  while (lane.client->State().readable < 65536 && GetTickCount64() < queued_deadline) Sleep(5);
  Require(lane.client->State().readable == 65536, "receive queue holds bounded backpressure");
  size_t peeked = 0;
  Require(lane.client->Recv(bytes, &peeked, true) == 0 && peeked == bytes.size() &&
      bytes[0] == pattern(0) && lane.client->State().readable == 65536, "relay peek does not consume");
  while (true) {
    Require(GetTickCount64() < deadline, "bounded data and EOF");
    const auto state = lane.client->State();
    size_t count = 0;
    const int result = lane.client->Recv(bytes, &count);
    Require(result == 0 || result == WSAEWOULDBLOCK, "relay receive");
    if (result == WSAEWOULDBLOCK) {
      lane.client->WaitForChange(state.revision, 100);
      continue;
    }
    if (count == 0) break;
    for (size_t i = 0; i < count; ++i)
      Require(bytes[i] == pattern(received++), "reassembled exact incoming bytes");
  }
  Require(received == kBytes, "read direction survives local send shutdown");
  size_t count = 9;
  Require(lane.client->Send(bytes, &count) == WSAESHUTDOWN && count == 0,
          "send after write shutdown is rejected");
  target.join();
  closesocket(listener);
  std::puts("PASS real TCP stream, metadata, 180000 ordered bytes each way, bounded queue, peek, both FINs and EOF");
}

void CloseAndReset() {
  uint16_t port = 0;
  SOCKET listener = Listener(&port);
  {
    Lane lane(Target(port));
    Require(lane.Connected().connected, "connected before close");
    SOCKET peer = accept(listener, nullptr, nullptr);
    Require(peer != INVALID_SOCKET, "accepted close witness");
    int timeout = 4000;
    Require(setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0, "bounded target receive");
    std::array<std::byte, 32000> data{};
    data.fill(static_cast<std::byte>('x'));
    size_t count = 0;
    Require(lane.client->Send(data, &count) == 0 && count == data.size(), "send before final close");
    lane.client->Close();
    size_t received = 0;
    while (true) {
      const int n = recv(peer, reinterpret_cast<char*>(data.data()), static_cast<int>(data.size()), 0);
      Require(n >= 0, "queued write delivered before final TCP close");
      if (!n) break;
      for (int i = 0; i < n; ++i) Require(data[i] == static_cast<std::byte>('x'), "queued data intact");
      received += static_cast<size_t>(n);
    }
    Require(received == data.size(), "all successfully queued bytes reached target before FIN");
    closesocket(peer);
  }
  {
    Lane lane(Target(port));
    Require(lane.Connected().connected, "connected before TCP reset");
    SOCKET peer = accept(listener, nullptr, nullptr);
    Require(peer != INVALID_SOCKET, "accepted reset witness");
    linger immediate{1, 0};
    Require(setsockopt(peer, SOL_SOCKET, SO_LINGER,
        reinterpret_cast<const char*>(&immediate), sizeof(immediate)) == 0, "enable real abortive close");
    closesocket(peer);
    auto state = lane.client->State();
    const auto deadline = GetTickCount64() + 5000;
    while (!state.error && GetTickCount64() < deadline) {
      lane.client->WaitForChange(state.revision, 100);
      state = lane.client->State();
    }
    Require(state.error == WSAECONNRESET, "retain actual connection reset");
  }
  closesocket(listener);
  std::puts("PASS queued successful write survives final close, real remote reset is reported");
}

void Terminate() {
  uint16_t port = 0;
  SOCKET listener = Listener(&port);
  {
    Lane lane(Target(port));
    Require(lane.Connected().connected, "connected before live revocation");
    SOCKET peer = accept(listener, nullptr, nullptr);
    Require(peer != INVALID_SOCKET, "real target on revocation");
    lane.Revoke();
    char value = 0;
    int timeout = 2500;
    Require(setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0, "bounded target EOF wait");
    Require(recv(peer, &value, 1, 0) == 0, "real outbound socket closed on authority revocation");
    closesocket(peer);
  }
  closesocket(listener);
  {
    Lane lane(Target(port));
    Require(lane.Connected().error == WSAECONNREFUSED, "preserve actual Winsock refusal");
  }
  std::puts("PASS live authority revocation closes the real socket, actual refusal propagates");
}

void Reject() {
  auto request = Target(36223);
  {
    Lane lane(request, false);
    Require(lane.Connected().error == WSAEACCES, "authority revoked before connect");
  }
  request.target.address[0] = 10;
  {
    Lane lane(request);
    Require(lane.Connected().error == WSAEACCES, "deny nonloopback address before network I/O");
  }
  request = Target(36223);
  request.source.port = 1234;
  {
    Lane lane(request);
    Require(lane.Connected().error == WSAEOPNOTSUPP, "explicit source can never change silently");
  }
  request = Target(36223);
  request.interface_index = 2;
  {
    Lane lane(request);
    Require(lane.Connected().error == WSAEOPNOTSUPP, "selected interface can never change silently");
  }
  request = Target(36223);
  request.options.mask = 1u << relay::kOptionCount;
  {
    Lane lane(request);
    Require(lane.Connected().error == WSAEACCES, "unknown option bits cannot expand authority");
  }
  request = Target(36223);
  request.options.values[0] = 1;
  {
    Lane lane(request);
    Require(lane.Connected().error == WSAEACCES, "unselected option payload is malformed");
  }
  std::puts("PASS exact authority, destination, explicit source and interface rejection");
}

void SocketOptions() {
  uint16_t port = 0;
  SOCKET listener = Listener(&port);
  auto request = Target(port);
  const auto no_delay = static_cast<uint32_t>(relay::Option::kNoDelay);
  request.options.mask = 1u << no_delay;
  request.options.values[no_delay] = 1;
  {
    Lane lane(request);
    Require(lane.Connected().connected, "initial native option before bind/connect");
    SOCKET peer = accept(listener, nullptr, nullptr);
    Require(peer != INVALID_SOCKET, "accept option witness");
    int32_t value = 0;
    const int option_result = lane.client->Control(relay::Option::kNoDelay, false, &value);
    if (option_result || value != 1) std::fprintf(stderr, "initial option: error=%d value=%d\n", option_result, value);
    Require(option_result == 0 && value == 1,
        "initial TCP_NODELAY exists on actual connected socket");
    value = 0;
    Require(lane.client->Control(relay::Option::kNoDelay, true, &value) == 0,
        "mutate actual connected TCP_NODELAY");
    value = 99;
    Require(lane.client->Control(relay::Option::kNoDelay, false, &value) == 0 && value == 0,
        "query observes real mutation, not the placeholder or caller buffer");
    SOCKET native = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Require(native != INVALID_SOCKET, "create independent option baseline");
    value = 300;
    const int native_result = setsockopt(native, IPPROTO_IP, IP_TTL,
        reinterpret_cast<const char*>(&value), sizeof(value));
    const int native_error = native_result == 0 ? 0 : WSAGetLastError();
    Require(native_error != 0 && lane.client->Control(relay::Option::kIpTtl, true, &value) == native_error,
        "actual invalid option result matches independent native socket");
    closesocket(native);
    std::array<char, 65536> full{};
    size_t sent = 0;
    while (sent < full.size()) {
      const int count = send(peer, full.data() + sent, static_cast<int>(full.size() - sent), 0);
      Require(count > 0, "fill the actual relay receive buffer");
      sent += static_cast<size_t>(count);
    }
    auto deadline = GetTickCount64() + 3000;
    while (lane.client->State().readable < full.size() && GetTickCount64() < deadline) Sleep(5);
    Require(lane.client->State().readable == full.size(), "receive buffer fully backpressured");
    value = 1;
    Require(lane.client->Control(relay::Option::kKeepAlive, true, &value) == 0,
        "real control remains usable behind full receive buffer");
    int receive_capacity = 1024;
    Require(setsockopt(peer, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receive_capacity),
        sizeof(receive_capacity)) == 0, "small real receive buffer for outbound backpressure");
    std::array<std::byte, 8192> bytes{};
    deadline = GetTickCount64() + 3000;
    ULONGLONG stalled = 0;
    while (GetTickCount64() < deadline) {
      size_t count = 0;
      const int result = lane.client->Send(bytes, &count);
      Require(result == 0 || result == WSAEWOULDBLOCK, "fill outgoing queue without fake progress");
      if (count) stalled = 0;
      else if (!stalled) stalled = GetTickCount64();
      else if (GetTickCount64() - stalled > 100) break;
      Sleep(1);
    }
    Require(stalled && lane.client->State().writable == 0, "outgoing socket and queues backpressured");
    std::array<std::thread, 4> queries;
    for (auto& query : queries) query = std::thread([&] {
      int32_t actual = 0;
      Require(lane.client->Control(relay::Option::kKeepAlive, false, &actual) == 0 && actual == 1,
          "serialized exact option replies through bidirectional data backpressure");
    });
    for (auto& query : queries) query.join();
    lane.Revoke();
    closesocket(peer);
  }
  closesocket(listener);
  std::puts("PASS real initial/connected socket options, native errors, concurrent ACKs and full-duplex backpressure");
}

void BackpressuredClose() {
  uint16_t port = 0;
  SOCKET listener = Listener(&port);
  Lane lane(Target(port));
  Require(lane.Connected().connected, "connect final-close backpressure witness");
  SOCKET peer = accept(listener, nullptr, nullptr);
  Require(peer != INVALID_SOCKET, "accept final-close witness");
  int buffer = 4096;
  Require(setsockopt(peer, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer),
      sizeof(buffer)) == 0, "set small actual receive buffer");
  std::array<std::byte, 8192> bytes{};
  bytes.fill(static_cast<std::byte>('z'));
  size_t accepted = 0;
  const auto deadline = GetTickCount64() + 4000;
  ULONGLONG stalled = 0;
  while (GetTickCount64() < deadline) {
    size_t count = 0;
    const int result = lane.client->Send(bytes, &count);
    Require(result == 0 || result == WSAEWOULDBLOCK, "only successful accepted sends are counted");
    accepted += count;
    if (count) stalled = 0;
    else if (!stalled) stalled = GetTickCount64();
    else if (GetTickCount64() - stalled > 200) break;
    Sleep(1);
  }
  Require(stalled && !lane.client->State().writable && accepted > 65536,
      "kernel, transport and client buffer are genuinely backpressured");
  lane.client->Close();
  Sleep(5000);
  int timeout = 7000;
  Require(setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
      sizeof(timeout)) == 0, "bounded independent peer drain");
  size_t received = 0;
  while (true) {
    const int count = recv(peer, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
    Require(count >= 0, "actual receive through final FIN");
    if (!count) break;
    for (int i = 0; i < count; ++i) Require(bytes[i] == static_cast<std::byte>('z'), "accepted byte integrity");
    received += static_cast<size_t>(count);
  }
  Require(received == accepted, "all successful sends survive close after five-second receiver stall");
  std::printf("PASS final close preserves %zu accepted bytes after five-second actual receiver stall\n", accepted);
  closesocket(peer);
  closesocket(listener);
}

void FullDuplexClose() {
  uint16_t port = 0;
  SOCKET listener = Listener(&port);
  {
    Lane lane(Target(port));
    Require(lane.Connected().connected, "connect duplex close witness");
    SOCKET peer = accept(listener, nullptr, nullptr);
    Require(peer != INVALID_SOCKET, "accept duplex close witness");
    int buffer = 4096, timeout = 12000;
    Require(setsockopt(peer, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer)) == 0 &&
        setsockopt(peer, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer)) == 0 &&
        setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0 &&
        setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0,
        "small real duplex buffers and bounded peer operations");
    std::atomic<bool> wrote{false};
    size_t received = 0;
    std::thread target([&] {
      std::array<char, 8192> bytes{};
      for (size_t sent = 0; sent < 1024 * 1024;) {
        const int n = send(peer, bytes.data(), static_cast<int>(bytes.size()), 0);
        Require(n > 0, "target completes actual one-megabyte write before reading");
        sent += static_cast<size_t>(n);
      }
      wrote.store(true);
      for (;;) {
        const int n = recv(peer, bytes.data(), static_cast<int>(bytes.size()), 0);
        Require(n >= 0, "target reads accepted outgoing bytes through actual EOF");
        if (!n) break;
        for (int i = 0; i < n; ++i) Require(bytes[i] == 'x', "duplex accepted byte integrity");
        received += static_cast<size_t>(n);
      }
    });
    std::array<std::byte, 8192> bytes{};
    bytes.fill(static_cast<std::byte>('x'));
    size_t accepted = 0;
    const auto deadline = GetTickCount64() + 5000;
    ULONGLONG stalled = 0;
    while (GetTickCount64() < deadline) {
      size_t count = 0;
      const int result = lane.client->Send(bytes, &count);
      Require(result == 0 || result == WSAEWOULDBLOCK, "duplex admitted sends counted exactly");
      accepted += count;
      if (count) stalled = 0;
      else if (!stalled) stalled = GetTickCount64();
      else if (GetTickCount64() - stalled > 300 && lane.client->State().readable == 65536) break;
      Sleep(1);
    }
    Require(stalled && !wrote.load() && lane.client->State().readable == 65536 &&
        !lane.client->State().writable && accepted > 65536, "both directions are actually blocked before final close");
    lane.client->Close();
    target.join();
    Require(wrote.load() && received == accepted, "final close discards inbound without losing accepted outgoing bytes");
    std::printf("PASS final close unblocks target one-megabyte write-before-read and preserves %zu accepted bytes plus EOF\n", accepted);
    closesocket(peer);
  }
  closesocket(listener);
}

}  // namespace

int main() {
  WSADATA data{};
  Require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock startup");
  EarlySource();
  Stream();
  CloseAndReset();
  Terminate();
  Reject();
  SocketOptions();
  BackpressuredClose();
  FullDuplexClose();
  ForkAdmission();
  for (int i = 0; i < 700 && relay::HasActiveWorkers(); ++i) Sleep(10);
  Require(!relay::HasActiveWorkers(), "all workers drain and permit a process-state snapshot");
  WSACleanup();
  return 0;
}
