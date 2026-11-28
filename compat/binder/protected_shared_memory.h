// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "protected_peers.h"

#include <cstdint>
#include <mutex>

namespace gesher::binder {

// One real ApplicationSharedMemory object per protected Android realm. The
// admitted system server publishes the object created by its framework native;
// no package name, PID lookup, path, object name or caller-supplied type grants
// permission. Keep the producer and the exact kernel object alive together.
class ProtectedSharedMemory final {
 public:
  ~ProtectedSharedMemory();
  ProtectedSharedMemory() = default;
  ProtectedSharedMemory(const ProtectedSharedMemory&) = delete;
  ProtectedSharedMemory& operator=(const ProtectedSharedMemory&) = delete;

  DWORD Register(const ProtectedPeers::Peer& producer, uint64_t source_handle, uint64_t size);

  // Returns a broker-owned staging handle in the exact consumer process. The
  // caller closes it through that retained process object after delivery or
  // failure. A client receives only FILE_READ_DATA/ATTRIBUTES + SYNCHRONIZE;
  // no writable OFD mapping or other caller-supplied handle accompanies it.
  DWORD Duplicate(const ProtectedPeers::Peer& producer,
                  const ProtectedPeers::Peer& consumer, uint64_t source_handle,
                  uint64_t* remote_handle);

 private:
  std::mutex mutex_;
  ProtectedPeers::Peer producer_;
  HANDLE object_ = nullptr;
  uint64_t size_ = 0;
};

}  // namespace gesher::binder
