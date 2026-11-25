// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Test-only loopback origin. Its counters are independent of WebView callbacks.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace worker_response_probe {

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

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace worker_response_probe
