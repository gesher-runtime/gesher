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
inline constexpr char kLongLegacyError[] = "Must use __system_property_read_callback() to read";
struct WaitTimeout { int64_t seconds; int64_t nanoseconds; };

// One area per device boot (GESHER_BINDER), or inherited single-process
// session. Calling this before spawning a child publishes the session name.
bool Initialize();
bool Get(const std::string& name, std::string* value, uint32_t* serial = nullptr);
bool Set(const std::string& name, const std::string& value);
Snapshot List();
uint32_t AreaSerial();
// A null name waits for the area generation. A false return leaves new_serial
// untouched; a null timeout waits indefinitely. Locks are released while idle.
bool Wait(const std::string* name, uint32_t old_serial, uint32_t* new_serial,
          const WaitTimeout* timeout);

}  // namespace gesher::properties
