// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace gesher::properties {

struct Value {
  std::string text;
  uint32_t serial = 0;
};
using Snapshot = std::map<std::string, Value>;

// One area per device boot (GESHER_BINDER), or inherited single-process
// session. Calling this before spawning a child publishes the session name.
bool Initialize();
bool Get(const std::string& name, std::string* value, uint32_t* serial = nullptr);
bool Set(const std::string& name, const std::string& value);
Snapshot List();
uint32_t AreaSerial();

}  // namespace gesher::properties
