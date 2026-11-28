// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace gesher::loopback_relay {

// A closed set of TCP controls. Neither a native level/name nor a handle is
// accepted from the application. Values and failures come from the real socket.
enum class Option : uint32_t {
  kNoDelay, kKeepAlive, kOobInline, kReceiveBuffer, kSendBuffer,
  kReceiveLowWater, kSendLowWater, kDontRoute, kKeepIdle, kKeepInterval,
  kKeepCount, kIpTos, kIpTtl, kIpv6Only, kIpv6Hops, kIpv6TrafficClass,
  kCount
};
inline constexpr size_t kOptionCount = static_cast<size_t>(Option::kCount);
struct Options {
  uint32_t mask = 0;
  int32_t values[kOptionCount]{};
};

struct Endpoint {
  uint32_t family = 0;             // 4 or 6
  uint8_t address[16]{};           // network byte order
  uint16_t port = 0;              // host byte order
  uint16_t reserved = 0;
  uint32_t scope = 0;
};

struct Request {
  Endpoint target;
  Endpoint source;
  uint32_t interface_index = 0;
  uint32_t reserved = 0;
  Options options;
};

static_assert(sizeof(Endpoint) == 28 && sizeof(Options) == 68 && sizeof(Request) == 132);

bool IsLoopback(const Endpoint& endpoint);
// Pending and active local Client workers, including a final close drain.
bool HasActiveWorkers();
// Includes idle or failed Client objects still referenced by descriptors.
bool HasProcessLocalState();
// Keep creation blocked from the successful try-lock through fork completion.
bool TryLockForkGate();
void UnlockForkGate();
void ResetForkGateAfterChild();

struct Snapshot {
  bool connecting = true;
  bool connected = false;
  bool read_eof = false;
  bool write_closed = false;
  int error = 0;                  // stable underlying Winsock result
  size_t readable = 0;
  size_t writable = 0;
  Endpoint local;
  Endpoint peer;
  uint64_t revision = 0;
};

class Client {
 public:
  struct Session;
  // open must authenticate the broker, send the fixed Request, and return an
  // owned pipe. open and wake run on a worker; wake must outlive the Client.
  // thread_leaving registers the actual thread with its native fork exit gate,
  // after all captured callbacks are destroyed but before Windows TLS teardown.
  // Pass nullptr explicitly only when the caller has no native fork facility.
  static std::shared_ptr<Client> Start(Request request,
      std::function<HANDLE(const Request&)> open, std::function<void()> wake,
      void (*thread_leaving)());
  ~Client();
  Snapshot State() const;
  bool WaitForChange(uint64_t revision, DWORD milliseconds);
  // Queue operations never block. Return a Winsock error or zero, with count
  // set on success (including a zero-sized operation, or a read at EOF).
  int Send(std::span<const std::byte> data, size_t* count);
  int Recv(std::span<std::byte> data, size_t* count, bool peek = false);
  int Shutdown(bool read, bool write);
  // Synchronous bounded control on the retained socket; WSA errors are returned
  // unchanged. A timeout retires the session so a late mutation cannot be used.
  int Control(Option option, bool set, int32_t* value);
  void Close();

 private:
  explicit Client(std::shared_ptr<Session> session);
  std::shared_ptr<Session> session_;
};

// The caller owns pipe and must authorize the exact process before dispatch.
// live must check the same enrolled generation and is rechecked during relay.
// Host Winsock endpoints never cross the transport.
void Serve(HANDLE pipe, const Request& request, const std::function<bool()>& live);

}  // namespace gesher::loopback_relay
