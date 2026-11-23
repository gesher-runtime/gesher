/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "hosted_drop.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gesher::windows {

// One table per admitted window, owned on its drop worker STA. Lease identifiers
// are meaningful only within that window's authenticated connection.
class DropLeases {
 public:
  struct Item {
    std::wstring name;
    bool directory;
  };
  struct Offer {
    uint64_t lease = 0;
    std::vector<Item> items;
  };

  DropLeases();
  ~DropLeases();
  DropLeases(const DropLeases&) = delete;
  DropLeases& operator=(const DropLeases&) = delete;

  // Retains files and directories obtained from the authenticated OLE payload.
  // An incomplete offer closes every handle and publishes no lease.
  int Retain(OleDropSource source, const std::vector<std::wstring>& paths, Offer* offer);
  bool Take(uint64_t lease);
  void Release(uint64_t lease);

  // Only relative components beneath an offered directory are accepted. The
  // returned file object is read-only and has its own kernel position/cursor.
  int Open(uint64_t lease, uint32_t item, std::wstring_view relative,
           bool directory, bool metadata, HANDLE* opened) const;

 private:
  class State;
  std::unique_ptr<State> state_;
};

}  // namespace gesher::windows
