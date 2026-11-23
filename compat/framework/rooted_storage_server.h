// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <gesher/rooted_storage.h>

#include <memory>

namespace gesher::rooted_storage {

// One isolated launch. Only the exact lowbox and its descendants in the same
// retained Windows job can request operations on these host-selected roots.
class Server {
 public:
  explicit Server(const Roots& roots);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  const std::wstring& name() const;
  DWORD Start(HANDLE child, HANDLE job);

 private:
  class State;
  std::unique_ptr<State> state_;
};

}  // namespace gesher::rooted_storage
