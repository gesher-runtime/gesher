// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Test-only loopback origin. Its counters are independent of WebView callbacks.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace worker_response_probe {

struct RequestRecord {
  std::string method, path, body;
  bool complete = false;
  uint64_t request_bytes = 0;
};

class LoopbackServer {
 public:
  LoopbackServer();
  ~LoopbackServer();
  LoopbackServer(const LoopbackServer&) = delete;
  LoopbackServer& operator=(const LoopbackServer&) = delete;
  bool Start(uint64_t deadline);
  bool Stop();
  bool Healthy() const;
  std::wstring Origin() const;
  unsigned Count(const std::string& path) const;
  unsigned TotalCount() const;
  unsigned Connections() const;
  uint64_t ReceivedBytes() const;
  uint64_t UnattributedBytes() const;
  std::vector<RequestRecord> Requests() const;
  bool SetRedirectOrigin(const std::wstring& origin);

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

// Real socket controls for complete, partial, empty and unclassifiable input.
bool CheckLoopbackRequestAccounting(uint64_t deadline);

}  // namespace worker_response_probe
