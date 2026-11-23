// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "protected_peer_role.h"

#include <cstddef>
#include <cstdint>

namespace gesher::binder::bootstrap_wire {

constexpr wchar_t kPipePrefix[] = LR"(\\.\pipe\gesher-binder-authority-)";
constexpr size_t kLocatorHexDigits = 32;
constexpr wchar_t kAuthorityService[] = L"GesherUsbAuthority";
constexpr uint32_t kMagic = 0x31425047;  // GPB1
constexpr uint32_t kVersion = 2;
constexpr uint32_t kPipeClientAccess = 0x0012018b;

enum class Operation : uint32_t {
  kHello = 1,
  kReady = 2,
  kRegister = 3,
  kRevoke = 4,
  kTakeListener = 5,
};

enum class Role : uint32_t {
  kRegistration = 0,
  kListeners = 1,
};

// All frames are fixed-size, little-endian. Read and write complete frames.
// Only binderd sends Hello, after authenticating the connected pipe server.
struct Hello {
  uint32_t magic;
  uint32_t version;
  Operation operation;
  Role role;
};
static_assert(sizeof(Hello) == 16);

// The authority sends Ready first, with all payload fields zero. Register
// and Revoke use a nonzero registration_id. Revoke has zero uid and handle
// and kClient role. Only Register can grant kContextManager.
// Register transfers a PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION
// | SYNCHRONIZE | READ_CONTROL process handle already duplicated into binderd.
// binderd owns and closes the transferred handle after consuming the frame.
// Once a send starts, the authority must abort the broker on I/O failure
// instead of closing the possibly reused target handle value.
struct Request {
  uint32_t magic;
  uint32_t version;
  Operation operation;
  uint32_t uid;
  uint64_t registration_id;
  uint64_t process_handle;
  PeerRole peer_role;
  uint32_t reserved;
};
static_assert(sizeof(Request) == 40);

// One reply to every accepted frame. status is a Win32 error code.
struct Reply {
  uint32_t magic;
  uint32_t version;
  uint32_t status;
  uint32_t reserved;
  uint64_t registration_id;
};
static_assert(sizeof(Reply) == 24);

// After the Ready acknowledgement on a listener connection, binderd sends
// one Take request and reads one reply at a time.
struct ListenerRequest {
  uint32_t magic;
  uint32_t version;
  Operation operation;
  uint32_t reserved;
};
static_assert(sizeof(ListenerRequest) == 16);

// Success transfers one disconnected synchronous server pipe handle already
// duplicated into binderd. An error reply has a zero listener_handle. The
// same transfer ownership rule as Register applies once the reply is sent.
struct ListenerReply {
  uint32_t magic;
  uint32_t version;
  uint32_t status;
  uint32_t reserved;
  uint64_t listener_handle;
};
static_assert(sizeof(ListenerReply) == 24);

}  // namespace gesher::binder::bootstrap_wire
