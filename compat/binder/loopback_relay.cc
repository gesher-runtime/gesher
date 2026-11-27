// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <winsock2.h>
#include <ws2tcpip.h>
#include "loopback_relay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>

#include "../posix/include/gesher/binder_bootstrap.h"

namespace gesher::loopback_relay {
namespace {

// Leave framing headroom even on a 4 KiB byte pipe. A nonblocking write can
// make no progress when the entire request is larger than its pipe buffer.
constexpr size_t kChunk = 2048, kQueue = 65536;
constexpr DWORD kConnectMs = 5000;
constexpr DWORD kPollMs = 10;
constexpr unsigned kMaxSessions = 96;
constexpr uint32_t kVersion = 1;
std::atomic<unsigned> worker_count{0}, client_count{0};
std::mutex fork_creation_gate;

enum ReplyPhase : uint32_t { kSourceReady = 1, kCompleted };
struct Reply {
  uint32_t version = kVersion;
  ReplyPhase phase = kCompleted;
  int32_t error = 0;
  Endpoint local, peer;
};
static_assert(sizeof(Reply) == 68);

// Linux exports identically named socket functions in the same binary.
struct NativeWinsock {
  bool ready = false;
  decltype(&::closesocket) closesocket = nullptr;
  decltype(&::htons) htons = nullptr;
  decltype(&::ntohs) ntohs = nullptr;
  decltype(&::getpeername) getpeername = nullptr;
  decltype(&::getsockname) getsockname = nullptr;
  decltype(&::connect) connect = nullptr;
  decltype(&::bind) bind = nullptr;
  decltype(&::select) select = nullptr;
  decltype(&::getsockopt) getsockopt = nullptr;
  decltype(&::send) send = nullptr;
  decltype(&::shutdown) shutdown = nullptr;
  decltype(&::recv) recv = nullptr;
  decltype(&::ioctlsocket) ioctlsocket = nullptr;
  decltype(&::setsockopt) setsockopt = nullptr;
  NativeWinsock() {
    HMODULE module = GetModuleHandleW(L"ws2_32.dll");
    if (!module) return;
#define LOAD(name) name = reinterpret_cast<decltype(name)>(reinterpret_cast<void*>(GetProcAddress(module, #name)))
    LOAD(closesocket);
    LOAD(htons);
    LOAD(ntohs);
    LOAD(getpeername);
    LOAD(getsockname);
    LOAD(connect);
    LOAD(bind);
    LOAD(select);
    LOAD(getsockopt);
    LOAD(send);
    LOAD(shutdown);
    LOAD(recv);
    LOAD(ioctlsocket);
    LOAD(setsockopt);
#undef LOAD
    ready = closesocket && htons && ntohs && getpeername && getsockname &&
        connect && bind && select && getsockopt && send && shutdown &&
        recv && ioctlsocket && setsockopt;
  }
};

const NativeWinsock& Net() {
  static const NativeWinsock native;
  return native;
}

struct PipeOwner {
  HANDLE value;
  ~PipeOwner() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct SocketOwner {
  SOCKET value;
  ~SocketOwner() { if (value != INVALID_SOCKET) Net().closesocket(value); }
};

bool BytesZero(const uint8_t* from, size_t count) {
  return std::all_of(from, from + count, [](uint8_t byte) { return byte == 0; });
}
bool SourceEmpty(const Endpoint& source) {
  return source.family == 0 && BytesZero(source.address, 16) &&
      source.port == 0 && source.scope == 0 && source.reserved == 0;
}
bool ReplyAck(HANDLE pipe, bool send, ULONGLONG deadline) {
  uint32_t value = send ? kVersion : 0;
  return binder_bootstrap::Transfer(pipe, &value, sizeof(value), send, deadline) && value == kVersion;
}
bool Same(const Endpoint& a, const Endpoint& b) {
  return a.family == b.family && a.port == b.port && a.scope == b.scope &&
      a.reserved == b.reserved && std::memcmp(a.address, b.address, 16) == 0;
}
bool WsaError(int32_t value) { return value >= WSABASEERR && value < WSABASEERR + 2000; }
bool Prepare(HANDLE pipe) {
  DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
  return SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) != FALSE;
}
bool Exchange(HANDLE pipe, Reply& reply, bool send, ULONGLONG deadline) {
  return binder_bootstrap::Transfer(pipe, &reply, sizeof(reply), send, deadline);
}

struct Queue {
  std::array<std::byte, kQueue> bytes{};
  size_t start = 0, used = 0;
  size_t Space() const { return bytes.size() - used; }
  size_t Push(std::span<const std::byte> data) {
    const size_t size = std::min(data.size(), Space());
    const size_t tail = (start + used) % bytes.size();
    const size_t first = std::min(size, bytes.size() - tail);
    if (size != 0) {
      std::memcpy(bytes.data() + tail, data.data(), first);
      std::memcpy(bytes.data(), data.data() + first, size - first);
    }
    used += size;
    return size;
  }
  size_t Read(std::span<std::byte> data, bool peek) {
    const size_t size = std::min(data.size(), used);
    const size_t first = std::min(size, bytes.size() - start);
    if (size != 0) {
      std::memcpy(data.data(), bytes.data() + start, first);
      std::memcpy(data.data() + first, bytes.data(), size - first);
    }
    if (!peek) {
      used -= size;
      start = (start + size) % bytes.size();
    }
    return size;
  }
  void Clear() { used = start = 0; }
};

enum Kind : uint32_t { kData = 1, kWriteEnd, kReadEnd, kError, kErrorAck, kWriteAck,
  kOption, kOptionReply, kCredit };
struct OptionMessage {
  uint32_t sequence, option, set;
  int32_t value, error;
};
static_assert(sizeof(OptionMessage) == 20);
struct Header { uint32_t kind, size; int32_t error; };
struct Frame { Header header{}; std::array<std::byte, kChunk> bytes{}; };
static_assert(offsetof(Frame, bytes) == sizeof(Header));

bool TransferSome(HANDLE pipe, Frame& frame, size_t end, size_t* pos, bool writing,
                  bool* progress) {
  auto* bytes = reinterpret_cast<std::byte*>(&frame);
  DWORD moved = 0;
  BOOL ok = writing ? WriteFile(pipe, bytes + *pos, static_cast<DWORD>(end - *pos), &moved, nullptr)
                    : ReadFile(pipe, bytes + *pos, static_cast<DWORD>(end - *pos), &moved, nullptr);
  if (!ok) return GetLastError() == ERROR_NO_DATA;
  *pos += moved;
  *progress |= moved != 0;
  return true;
}

struct Channel {
  explicit Channel(HANDLE handle) : pipe(handle) {}
  HANDLE pipe;
  Frame incoming, outgoing;
  size_t read = 0, written = 0;
  bool sending = false;
  bool received = false;

  bool Step(bool* progress) {
    if (sending) {
      const size_t end = sizeof(Header) + outgoing.header.size;
      if (!TransferSome(pipe, outgoing, end, &written, true, progress)) return false;
      if (written == end) sending = false;
    }
    if (received) return true;
    if (read < sizeof(Header)) {
      if (!TransferSome(pipe, incoming, sizeof(Header), &read, false, progress)) return false;
      if (read != sizeof(Header)) return true;
      const Header h = incoming.header;
      if (h.kind == kCredit ? (h.size != sizeof(uint32_t) || h.error != 0) :
          (h.kind == kOption || h.kind == kOptionReply) ?
          (h.size != sizeof(OptionMessage) || h.error != 0) :
          h.kind == kData ? (h.size == 0 || h.size > kChunk || h.error != 0) :
          (h.size != 0 || (h.kind == kError ? !WsaError(h.error) :
            ((h.kind != kWriteEnd && h.kind != kReadEnd && h.kind != kErrorAck &&
              h.kind != kWriteAck) ||
             h.error != 0)))) return false;
    }
    const size_t end = sizeof(Header) + incoming.header.size;
    if (read < end && !TransferSome(pipe, incoming, end, &read, false, progress)) return false;
    received = read == end;
    return true;
  }

  void Pop() { received = false; read = 0; }
  void Offer(Kind kind, size_t count = 0, int error = 0) {
    outgoing.header = {kind, static_cast<uint32_t>(count), error};
    sending = true;
    written = 0;
  }
};

bool NativeOption(Option option, int* level, int* name) {
  switch (option) {
    case Option::kNoDelay: *level = IPPROTO_TCP; *name = TCP_NODELAY; return true;
    case Option::kKeepAlive: *level = SOL_SOCKET; *name = SO_KEEPALIVE; return true;
    case Option::kOobInline: *level = SOL_SOCKET; *name = SO_OOBINLINE; return true;
    case Option::kReceiveBuffer: *level = SOL_SOCKET; *name = SO_RCVBUF; return true;
    case Option::kSendBuffer: *level = SOL_SOCKET; *name = SO_SNDBUF; return true;
    case Option::kReceiveLowWater: *level = SOL_SOCKET; *name = SO_RCVLOWAT; return true;
    case Option::kSendLowWater: *level = SOL_SOCKET; *name = SO_SNDLOWAT; return true;
    case Option::kDontRoute: *level = SOL_SOCKET; *name = SO_DONTROUTE; return true;
    case Option::kKeepIdle: *level = IPPROTO_TCP; *name = TCP_KEEPIDLE; return true;
    case Option::kKeepInterval: *level = IPPROTO_TCP; *name = TCP_KEEPINTVL; return true;
    case Option::kKeepCount: *level = IPPROTO_TCP; *name = TCP_KEEPCNT; return true;
    case Option::kIpTos: *level = IPPROTO_IP; *name = IP_TOS; return true;
    case Option::kIpTtl: *level = IPPROTO_IP; *name = IP_TTL; return true;
    case Option::kIpv6Only: *level = IPPROTO_IPV6; *name = IPV6_V6ONLY; return true;
    case Option::kIpv6Hops: *level = IPPROTO_IPV6; *name = IPV6_UNICAST_HOPS; return true;
    case Option::kIpv6TrafficClass: *level = IPPROTO_IPV6; *name = IPV6_TCLASS; return true;
    default: return false;
  }
}

int SocketControl(SOCKET socket, Option option, bool set, int32_t* value) {
  int level = 0, name = 0;
  if (!NativeOption(option, &level, &name)) return WSAENOPROTOOPT;
  int length = sizeof(*value);
  if (!set) *value = 0;
  const int result = set ? Net().setsockopt(socket, level, name,
      reinterpret_cast<const char*>(value), length) : Net().getsockopt(socket, level, name,
      reinterpret_cast<char*>(value), &length);
  if (result != 0) return WSAGetLastError();
  // A successful mutation acknowledges the actual normalized socket value;
  // this is also the value restored if the descriptor later disconnects.
  if (set) return SocketControl(socket, option, false, value);
  // Microsoft's TCP provider may return a one-byte BOOLEAN for TCP_NODELAY.
  // The destination was zeroed above, so the remaining bytes are defined.
  return length == sizeof(*value) || (!set && length == 1 &&
      (option == Option::kNoDelay || option == Option::kKeepAlive ||
       option == Option::kOobInline || option == Option::kDontRoute)) ? 0 : WSAEINVAL;
}

bool ValidOptions(const Options& options) {
  if (options.mask >> kOptionCount) return false;
  for (size_t i = 0; i < kOptionCount; ++i)
    if (!(options.mask & (1u << i)) && options.values[i]) return false;
  return true;
}

int Pack(const Endpoint& endpoint, sockaddr_storage* address) {
  if (endpoint.family == 4) {
    auto* v4 = reinterpret_cast<sockaddr_in*>(address);
    v4->sin_family = AF_INET;
    v4->sin_port = Net().htons(endpoint.port);
    std::memcpy(&v4->sin_addr, endpoint.address, 4);
    return sizeof(*v4);
  }
  auto* v6 = reinterpret_cast<sockaddr_in6*>(address);
  v6->sin6_family = AF_INET6;
  v6->sin6_port = Net().htons(endpoint.port);
  v6->sin6_scope_id = endpoint.scope;
  std::memcpy(&v6->sin6_addr, endpoint.address, 16);
  return sizeof(*v6);
}

Endpoint Name(SOCKET socket, bool peer) {
  sockaddr_storage address{};
  int size = sizeof(address);
  Endpoint value{};
  if ((peer ? Net().getpeername(socket, reinterpret_cast<sockaddr*>(&address), &size)
            : Net().getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size)) != 0) return value;
  if (address.ss_family == AF_INET && size == sizeof(sockaddr_in)) {
    const auto* v4 = reinterpret_cast<const sockaddr_in*>(&address);
    value.family = 4;
    value.port = Net().ntohs(v4->sin_port);
    std::memcpy(value.address, &v4->sin_addr, 4);
  } else if (address.ss_family == AF_INET6 && size == sizeof(sockaddr_in6)) {
    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&address);
    value.family = 6;
    value.scope = v6->sin6_scope_id;
    value.port = Net().ntohs(v6->sin6_port);
    std::memcpy(value.address, &v6->sin6_addr, 16);
  }
  return value;
}

bool PipeLive(HANDLE pipe) {
  DWORD available = 0;
  return PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) != FALSE;
}

int ConnectHost(SOCKET socket, const Endpoint& endpoint, ULONGLONG deadline, HANDLE pipe,
                const std::function<bool()>& live) {
  sockaddr_storage address{};
  const int size = Pack(endpoint, &address);
  if (Net().connect(socket, reinterpret_cast<sockaddr*>(&address), size) == 0) return 0;
  const int error = WSAGetLastError();
  if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) return error;
  while (GetTickCount64() < deadline) {
    if (!live() || !PipeLive(pipe)) return WSAECONNABORTED;
    fd_set writable, failed;
    FD_ZERO(&writable); FD_SET(socket, &writable);
    FD_ZERO(&failed); FD_SET(socket, &failed);
    timeval wait{0, 10000};
    const int result = Net().select(0, nullptr, &writable, &failed, &wait);
    if (result < 0) return WSAGetLastError();
    if (result > 0) {
      int code = 0, length = sizeof(code);
      if (Net().getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &length) != 0)
        return WSAGetLastError();
      return code;
    }
  }
  return WSAETIMEDOUT;
}

void RelayHost(HANDLE pipe, SOCKET socket, const std::function<bool()>& live) {
  Channel channel(pipe);
  Queue output;
  size_t read_credit = kQueue;
  uint32_t return_credit = 0;
  bool write_closed = false, peer_eof = false, discard_read = false, write_ack = false;
  bool write_requested = false;
  ULONGLONG next_check = 0, terminal_deadline = 0;
  uint32_t option_sequence = 0;
  bool option_reply = false;
  OptionMessage control{};
  while (true) {
    const auto now = GetTickCount64();
    if (now >= next_check) {
      if (!live()) return;
      next_check = now + 50;
    }
    if (terminal_deadline && now >= terminal_deadline) return;
    bool progress = false;
    if (!channel.Step(&progress)) return;
    if (terminal_deadline) {
      if (channel.received) {
        if (channel.incoming.header.kind == kErrorAck && !channel.sending) return;
        channel.Pop();
      }
      if (!progress) Sleep(kPollMs);
      continue;
    }
    int error = 0;
    if (channel.received) {
      const Header h = channel.incoming.header;
      if (h.kind == kData && !write_requested) {
        if (h.size > output.Space()) return;
        output.Push({channel.incoming.bytes.data(), h.size});
        channel.Pop();
      } else if (h.kind == kWriteEnd && !write_requested) {
        write_requested = true;
        channel.Pop();
      } else if (h.kind == kReadEnd && !discard_read) {
        discard_read = true;
        channel.Pop();
      } else if (h.kind == kCredit) {
        uint32_t credit = 0;
        std::memcpy(&credit, channel.incoming.bytes.data(), sizeof(credit));
        if (!credit || credit > kQueue - read_credit) return;
        read_credit += credit;
        channel.Pop();
      } else if (h.kind == kOption && !option_reply) {
        std::memcpy(&control, channel.incoming.bytes.data(), sizeof(control));
        if (!control.sequence || option_sequence == UINT32_MAX ||
            control.sequence != option_sequence + 1 || control.option >= kOptionCount ||
            control.set > 1 || control.error || (!control.set && control.value)) return;
        option_sequence = control.sequence;
        control.error = SocketControl(socket, static_cast<Option>(control.option),
            control.set != 0, &control.value);
        option_reply = true;
        channel.Pop();
      } else {
        return;
      }
    }
    if (output.used && !error) {
      std::array<std::byte, kChunk> bytes{};
      const size_t size = output.Read(bytes, true);
      const int count = Net().send(socket, reinterpret_cast<const char*>(bytes.data()),
          static_cast<int>(size), 0);
      if (count > 0) {
        output.Read({bytes.data(), static_cast<size_t>(count)}, false);
        return_credit += static_cast<uint32_t>(count);
        progress = true;
      } else if (count == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        error = WSAGetLastError();
      }
    }
    if (write_requested && !write_closed && !output.used && !error) {
      if (Net().shutdown(socket, SD_SEND) == SOCKET_ERROR) error = WSAGetLastError();
      else write_ack = true;
      write_closed = true;
    }
    if (!channel.sending && option_reply && !error) {
      std::memcpy(channel.outgoing.bytes.data(), &control, sizeof(control));
      channel.Offer(kOptionReply, sizeof(control));
      option_reply = false;
      progress = true;
    }
    if (!channel.sending && write_ack && !error) {
      channel.Offer(kWriteAck);
      write_ack = false;
      progress = true;
    }
    if (!channel.sending && return_credit && !error) {
      std::memcpy(channel.outgoing.bytes.data(), &return_credit, sizeof(return_credit));
      channel.Offer(kCredit, sizeof(return_credit));
      return_credit = 0;
      progress = true;
    }
    if (!channel.sending && error == 0 && !peer_eof && (read_credit || discard_read)) {
      const int n = Net().recv(socket, reinterpret_cast<char*>(channel.outgoing.bytes.data()),
          static_cast<int>(discard_read ? kChunk : std::min(kChunk, read_credit)), 0);
      if (n > 0) {
        progress = true;
        if (!discard_read) {
          channel.Offer(kData, static_cast<size_t>(n));
          read_credit -= static_cast<size_t>(n);
        }
      } else if (n == 0) {
        peer_eof = true;
        if (!discard_read) channel.Offer(kWriteEnd);
      } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
        error = WSAGetLastError();
      }
      progress |= channel.sending;
    }
    if (error) {
      // Preserve a partially written frame. If it cannot finish, the client
      // reports transport failure, never partial success or fabricated EOF.
      while (channel.sending && GetTickCount64() < now + 100) {
        if (!channel.Step(&progress)) return;
        if (channel.sending) Sleep(1);
      }
      if (channel.sending) return;
      channel.Offer(kError, 0, error);
      terminal_deadline = now + 250;
    }
    if (!progress) Sleep(kPollMs);
  }
}

}  // namespace

bool HasActiveWorkers() { return worker_count.load(std::memory_order_acquire) != 0; }
bool HasProcessLocalState() {
  return client_count.load(std::memory_order_acquire) != 0 || HasActiveWorkers();
}
bool TryLockForkGate() {
  if (!fork_creation_gate.try_lock()) return false;
  if (!HasProcessLocalState()) return true;
  fork_creation_gate.unlock();
  return false;
}
void UnlockForkGate() { fork_creation_gate.unlock(); }
void ResetForkGateAfterChild() { new (&fork_creation_gate) std::mutex; }

bool IsLoopback(const Endpoint& endpoint) {
  if (!endpoint.port || endpoint.reserved || endpoint.scope) return false;
  const auto* a = endpoint.address;
  const auto v4 = [](const uint8_t* ip) {
    return ip[0] == 127 && ip[1] == 0 && ip[2] == 0 && ip[3] == 1;
  };
  if (endpoint.family == 4) return v4(a) && BytesZero(a + 4, 12);
  return endpoint.family == 6 &&
      ((BytesZero(a, 15) && a[15] == 1) ||
       (BytesZero(a, 10) && a[10] == 255 && a[11] == 255 && v4(a + 12)));
}

struct Client::Session {
  Session(Request request, std::function<HANDLE(const Request&)> open, std::function<void()> wake)
      : request(request), open(std::move(open)), wake(std::move(wake)) {}
  const Request request;
  const std::function<HANDLE(const Request&)> open;
  const std::function<void()> wake;
  mutable std::mutex mutex;
  std::condition_variable changed;
  Snapshot state;
  Queue input, output;
  bool closed = false, write_acked = false;
  bool drain_on_close = false;
  bool stop_read = false, stop_write = false, read_shutdown = false;
  OptionMessage control{};
  uint32_t next_control = 0;
  bool control_pending = false, control_sent = false, control_done = false;
  size_t send_credit = kQueue;
  uint32_t return_credit = 0;

  void Notify() {
    changed.notify_all();
    if (wake) wake();
  }
  void Fail(int error) {
    {
      std::lock_guard lock(mutex);
      if (closed) return;
      state.connecting = false;
      state.error = error;
      state.write_closed = true;
      ++state.revision;
    }
    Notify();
  }
  void Run();
};

void Client::Session::Run() {
  {
    std::lock_guard lock(mutex);
    if (closed) return;
  }
  PipeOwner socket_pipe{open(request)};
  if (!socket_pipe.value || socket_pipe.value == INVALID_HANDLE_VALUE) { Fail(WSAEACCES); return; }
  const HANDLE pipe = socket_pipe.value;
  if (!Prepare(pipe)) { Fail(WSAECONNABORTED); return; }
  const auto deadline = GetTickCount64() + kConnectMs + 750;
  auto read_reply = [&](Reply* reply) {
    auto* cursor = reinterpret_cast<BYTE*>(reply);
    for (size_t read = 0; read < sizeof(*reply);) {
      {
        std::lock_guard lock(mutex);
        if (closed) return false;
      }
      DWORD moved = 0;
      if (GetTickCount64() >= deadline ||
          (!ReadFile(pipe, cursor + read, static_cast<DWORD>(sizeof(*reply) - read), &moved, nullptr) &&
           GetLastError() != ERROR_NO_DATA)) return false;
      if (!moved) Sleep(kPollMs);
      read += moved;
    }
    return reply->version == kVersion && (!reply->error || WsaError(reply->error));
  };
  auto acknowledge = [&] { return ReplyAck(pipe, true, GetTickCount64() + 250); };
  Reply reply;
  Endpoint source;
  if (!read_reply(&reply)) { Fail(WSAECONNABORTED); return; }
  if (reply.phase == kSourceReady) {
    if (reply.error || !IsLoopback(reply.local) || reply.local.family != request.target.family ||
        !SourceEmpty(reply.peer)) { Fail(WSAEINVAL); return; }
    source = reply.local;
    {
      std::lock_guard lock(mutex);
      if (closed) return;
      state.local = source;
      ++state.revision;
    }
    Notify();
    if (!acknowledge() || !read_reply(&reply)) { Fail(WSAECONNABORTED); return; }
  }
  if (reply.phase != kCompleted || !acknowledge()) { Fail(WSAECONNABORTED); return; }
  if (reply.error) { Fail(reply.error); return; }
  if (!source.family || !Same(reply.local, source) || !Same(reply.peer, request.target)) {
    Fail(WSAEINVAL); return;
  }
  {
    std::lock_guard lock(mutex);
    if (closed) return;
    state.connecting = false;
    state.connected = true;
    state.local = reply.local;
    state.peer = reply.peer;
    ++state.revision;
  }
  Notify();
  Channel channel(pipe);
  while (true) {
    bool progress = false;
    if (!channel.Step(&progress)) { Fail(WSAECONNRESET); return; }
    bool notify = false;
    int error = 0;
    {
      std::unique_lock lock(mutex);
      if (closed && (!drain_on_close || write_acked)) return;
      if (channel.received) {
        const Header h = channel.incoming.header;
        if (h.kind == kData && closed) {
          channel.Pop();
        } else if (h.kind == kData && !state.read_eof) {
          if (input.Space() >= h.size) {
            input.Push({channel.incoming.bytes.data(), h.size});
            channel.Pop();
            notify = true;
          }
        } else if (h.kind == kData && read_shutdown) {
          // If local read shutdown was already sent, in-flight bytes may
          // still arrive. Discard only those; peer data after its FIN is invalid.
          channel.Pop();
        } else if (h.kind == kWriteEnd) {
          state.read_eof = true;
          channel.Pop();
          notify = true;
        } else if (h.kind == kWriteAck && state.write_closed && !write_acked) {
          write_acked = true;
          channel.Pop();
          notify = true;
        } else if (h.kind == kError) {
          error = h.error;
        } else if (h.kind == kCredit) {
          uint32_t credit = 0;
          std::memcpy(&credit, channel.incoming.bytes.data(), sizeof(credit));
          if (!credit || credit > kQueue - send_credit) error = WSAECONNABORTED;
          else { send_credit += credit; channel.Pop(); notify = true; }
        } else if (h.kind == kOptionReply && control_pending && control_sent && !control_done) {
          OptionMessage option_reply{};
          std::memcpy(&option_reply, channel.incoming.bytes.data(), sizeof(option_reply));
          if (option_reply.sequence != control.sequence || option_reply.option != control.option ||
              option_reply.set != control.set || (option_reply.error && !WsaError(option_reply.error))) {
            error = WSAECONNABORTED;
          } else {
            control = option_reply;
            control_done = true;
            channel.Pop();
            notify = true;
          }
        } else {
          error = WSAECONNABORTED;
        }
      }
      if (!error && !channel.sending) {
        if (control_pending && !control_sent) {
          std::memcpy(channel.outgoing.bytes.data(), &control, sizeof(control));
          channel.Offer(kOption, sizeof(control));
          control_sent = true;
          progress = true;
        } else if (return_credit) {
          std::memcpy(channel.outgoing.bytes.data(), &return_credit, sizeof(return_credit));
          channel.Offer(kCredit, sizeof(return_credit));
          return_credit = 0;
          progress = true;
        } else if (stop_read) {
          // Read shutdown must bypass outbound backpressure. A target may be
          // writing before it reads our accepted bytes; the host must discard
          // those inbound bytes even after our receive credit is exhausted.
          channel.Offer(kReadEnd);
          stop_read = false;
          progress = true;
        } else if (output.used && send_credit) {
          const size_t size = output.Read({channel.outgoing.bytes.data(),
              std::min(kChunk, send_credit)}, false);
          channel.Offer(kData, size);
          send_credit -= size;
          notify = true;
        } else if (stop_write && !output.used) {
          channel.Offer(kWriteEnd);
          stop_write = false;
          progress = true;
        }
      }
      if (notify) ++state.revision;
      if (!progress && !notify && !error) {
        const auto revision = state.revision;
        changed.wait_for(lock, std::chrono::milliseconds(kPollMs),
                         [&] { return revision != state.revision; });
      }
    }
    if (error) {
      const bool remote_error = channel.incoming.header.kind == kError;
      Fail(error);
      if (remote_error) {
        const auto ack_deadline = GetTickCount64() + 200;
        while (channel.sending && GetTickCount64() < ack_deadline) {
          if (!channel.Step(&progress)) return;
          if (channel.sending) Sleep(1);
        }
        if (channel.sending) return;
        channel.Offer(kErrorAck);
        while (channel.sending && GetTickCount64() < ack_deadline) {
          if (!channel.Step(&progress)) return;
          if (channel.sending) Sleep(1);
        }
      }
      return;
    }
    if (notify) Notify();
  }
}

Client::Client(std::shared_ptr<Session> session) : session_(std::move(session)) {
  client_count.fetch_add(1, std::memory_order_release);
}
Client::~Client() {
  Close();
  session_.reset();
  client_count.fetch_sub(1, std::memory_order_release);
}

std::shared_ptr<Client> Client::Start(Request request, std::function<HANDLE(const Request&)> open,
                                     std::function<void()> wake) {
  std::unique_lock creation(fork_creation_gate);
  auto session = std::make_shared<Session>(request, std::move(open), std::move(wake));
  auto owner = std::shared_ptr<Client>(new Client(session));
  // The client source and integration callbacks may be linked into a
  // dynamically unloaded Android DLL. Keep that image mapped through the
  // actual worker exit, including destruction of captured callbacks.
  struct Worker {
    std::shared_ptr<Session> session;
    HMODULE module;
    static DWORD WINAPI Run(LPVOID parameter) {
      HMODULE module;
      {
        std::unique_ptr<Worker> context(static_cast<Worker*>(parameter));
        module = context->module;
        context->session->Run();
      }
      // No relay state, locks or callbacks survive this release. The only
      // remaining instruction enters the kernel's module/thread release.
      worker_count.fetch_sub(1, std::memory_order_release);
      FreeLibraryAndExitThread(module, 0);
    }
  };
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
          reinterpret_cast<LPCWSTR>(&Worker::Run), &module)) {
    creation.unlock();
    session->Fail(WSAENOBUFS);
    return owner;
  }
  auto* context = new (std::nothrow) Worker{session, module};
  worker_count.fetch_add(1, std::memory_order_release);
  HANDLE worker = context ? CreateThread(nullptr, 0, &Worker::Run, context, 0, nullptr) : nullptr;
  creation.unlock();
  if (worker) CloseHandle(worker);
  else {
    delete context;
    worker_count.fetch_sub(1, std::memory_order_release);
    FreeLibrary(module);
    session->Fail(WSAENOBUFS);
  }
  return owner;
}

Snapshot Client::State() const {
  std::lock_guard lock(session_->mutex);
  Snapshot result = session_->state;
  result.readable = session_->input.used;
  result.writable = result.connected && !result.write_closed && !result.error && !session_->closed
      ? session_->output.Space() : 0;
  return result;
}
bool Client::WaitForChange(uint64_t revision, DWORD milliseconds) {
  std::unique_lock lock(session_->mutex);
  auto ready = [&] { return session_->closed || session_->state.revision != revision; };
  if (milliseconds == INFINITE) { session_->changed.wait(lock, ready); return true; }
  return session_->changed.wait_for(lock, std::chrono::milliseconds(milliseconds), ready);
}

int Client::Send(std::span<const std::byte> data, size_t* count) {
  *count = 0;
  {
    std::lock_guard lock(session_->mutex);
    const auto& state = session_->state;
    if (session_->closed) return WSAENOTSOCK;
    if (state.error) return state.error;
    if (state.connecting) return WSAEWOULDBLOCK;
    if (state.write_closed) return WSAESHUTDOWN;
    if (data.empty()) return 0;
    *count = session_->output.Push(data);
    if (!*count) return WSAEWOULDBLOCK;
    ++session_->state.revision;
  }
  session_->Notify();
  return 0;
}

int Client::Recv(std::span<std::byte> data, size_t* count, bool peek) {
  *count = 0;
  {
    std::lock_guard lock(session_->mutex);
    const auto& state = session_->state;
    if (session_->closed) return WSAENOTSOCK;
    if (data.empty()) return 0;
    *count = session_->input.Read(data, peek);
    if (!*count) {
      if (state.error) return state.error;
      return state.read_eof ? 0 : WSAEWOULDBLOCK;
    }
    if (peek) return 0;
    session_->return_credit += static_cast<uint32_t>(*count);
    ++session_->state.revision;
  }
  session_->Notify();
  return 0;
}

int Client::Shutdown(bool read, bool write) {
  {
    std::lock_guard lock(session_->mutex);
    auto& state = session_->state;
    if (session_->closed) return WSAENOTSOCK;
    if (state.error) return state.error;
    if (!state.connected) return WSAENOTCONN;
    if (read && !session_->read_shutdown) {
      session_->read_shutdown = true;
      session_->input.Clear();
      if (!state.read_eof) session_->stop_read = true;
      state.read_eof = true;
    }
    if (write && !state.write_closed) {
      state.write_closed = true;
      session_->stop_write = true;
    }
    ++state.revision;
  }
  session_->Notify();
  return 0;
}

int Client::Control(Option option, bool set, int32_t* value) {
  if (!value || static_cast<uint32_t>(option) >= kOptionCount) return WSAEINVAL;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  std::unique_lock lock(session_->mutex);
  auto terminal = [&] { return session_->closed || session_->state.error != 0; };
  if (!session_->changed.wait_until(lock, deadline,
      [&] { return terminal() || !session_->control_pending; })) return WSAEWOULDBLOCK;
  if (session_->closed) return WSAENOTSOCK;
  if (session_->state.error) return session_->state.error;
  if (!session_->state.connected) return WSAENOTCONN;
  if (session_->next_control == UINT32_MAX) return WSAENOBUFS;
  session_->control = {++session_->next_control, static_cast<uint32_t>(option),
      set ? 1u : 0u, set ? *value : 0, 0};
  session_->control_pending = true;
  session_->control_sent = session_->control_done = false;
  ++session_->state.revision;
  lock.unlock();
  session_->Notify();
  lock.lock();
  if (!session_->changed.wait_until(lock, deadline,
      [&] { return terminal() || session_->control_done; })) {
    // The peer may already have applied the operation. Retire the connection,
    // rather than allowing a later caller to observe an unacknowledged change.
    lock.unlock();
    Close();
    return WSAETIMEDOUT;
  }
  int error = session_->closed ? WSAENOTSOCK : session_->state.error;
  if (!error) {
    error = session_->control.error;
    if (!error) *value = session_->control.value;
  }
  session_->control_pending = session_->control_done = session_->control_sent = false;
  lock.unlock();
  session_->Notify();
  return error;
}

void Client::Close() {
  {
    std::lock_guard lock(session_->mutex);
    if (session_->closed) return;
    session_->closed = true;
    if (session_->state.connected && !session_->state.error && !session_->write_acked) {
      // A successful send owns those bytes until the real TCP stack accepts
      // them. A fixed grace period would silently truncate a backpressured
      // stream. The pinned worker remains within its admitted bounded slot
      // until kernel acceptance, a real error, or application retirement.
      session_->drain_on_close = true;
      if (!session_->read_shutdown) {
        session_->read_shutdown = true;
        session_->stop_read = true;
      }
      if (!session_->state.write_closed) session_->stop_write = true;
    }
    session_->state.connecting = false;
    session_->state.connected = false;
    session_->state.error = WSAENOTSOCK;
    session_->state.write_closed = true;
    ++session_->state.revision;
  }
  session_->Notify();
}

void Serve(HANDLE pipe, const Request& request, const std::function<bool()>& live) {
  if (!Prepare(pipe)) return;
  const auto deadline = GetTickCount64() + kConnectMs;
  Reply reply;
  static std::atomic<unsigned> sessions{0};
  if (sessions.fetch_add(1) >= kMaxSessions) { sessions.fetch_sub(1); return; }
  struct Slot { std::atomic<unsigned>& count; ~Slot() { count.fetch_sub(1); } } slot{sessions};
  auto send_reply = [&] {
    const auto response_deadline = GetTickCount64() + 250;
    return Exchange(pipe, reply, true, response_deadline) && ReplyAck(pipe, false, response_deadline);
  };
  if (!live() || !IsLoopback(request.target) || request.reserved || !ValidOptions(request.options))
    reply.error = WSAEACCES;
  else if (!SourceEmpty(request.source) || request.interface_index) reply.error = WSAEOPNOTSUPP;
  if (reply.error) { send_reply(); return; }
  static std::once_flag once;
  static int started = WSASYSNOTREADY;
  std::call_once(once, [] { WSADATA data{}; started = WSAStartup(MAKEWORD(2, 2), &data); });
  SocketOwner socket{INVALID_SOCKET};
  if (started || !Net().ready) reply.error = started ? started : WSASYSNOTREADY;
  else {
    socket.value = WSASocketW(request.target.family == 4 ? AF_INET : AF_INET6,
        SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (socket.value == INVALID_SOCKET) reply.error = WSAGetLastError();
    else {
      u_long nonblocking = 1;
      DWORD dual = 0;
      if (Net().ioctlsocket(socket.value, FIONBIO, &nonblocking) != 0 ||
          (request.target.family == 6 && Net().setsockopt(socket.value, IPPROTO_IPV6, IPV6_V6ONLY,
          reinterpret_cast<const char*>(&dual), sizeof(dual)) != 0)) reply.error = WSAGetLastError();
      else {
        for (size_t i = 0; i < kOptionCount && !reply.error; ++i) {
          if (!(request.options.mask & (1u << i))) continue;
          int32_t value = request.options.values[i];
          reply.error = SocketControl(socket.value, static_cast<Option>(i), true, &value);
        }
      }
      if (!reply.error) {
        sockaddr_storage address{};
        Endpoint source = request.target;
        source.port = 0;
        const int size = Pack(source, &address);
        if (Net().bind(socket.value, reinterpret_cast<sockaddr*>(&address), size) != 0) {
          reply.error = WSAGetLastError();
        } else {
          const Endpoint bound = Name(socket.value, false);
          if (!IsLoopback(bound) || bound.family != source.family || !live()) {
            reply.error = WSAEACCES;
          } else {
            Reply early{kVersion, kSourceReady, 0, bound, {}};
            const auto source_deadline = GetTickCount64() + 250;
            if (!Exchange(pipe, early, true, source_deadline) ||
                !ReplyAck(pipe, false, source_deadline) || !live()) return;
            reply.error = ConnectHost(socket.value, request.target, deadline, pipe, live);
            if (!reply.error) {
              reply.local = Name(socket.value, false);
              reply.peer = Name(socket.value, true);
              if (!live() || !Same(reply.local, bound) || !Same(reply.peer, request.target))
                reply.error = WSAEACCES;
            }
          }
        }
      }
    }
  }
  if (!send_reply() || reply.error || !live()) return;
  RelayHost(pipe, socket.value, live);
}

}  // namespace gesher::loopback_relay
