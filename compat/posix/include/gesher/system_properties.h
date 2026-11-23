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
// A process-local area identity plus its published generation. Area identities
// remain valid for the process lifetime. An empty identity must not be cached.
struct Revision {
  const void* area = nullptr;
  uint32_t generation = 0;
  bool operator==(const Revision&) const = default;
};
// Observes only the aligned published generation, without the area mutex or
// decoding its payload. Detects a change of GESHER_BINDER/session as well.
Revision CurrentRevision();
// Get for |count| names with one area lookup (an unset name gives "").
// An optional revision describes exactly the snapshot used for these values.
void GetAll(const std::string* names, std::string* values, size_t count, Revision* revision = nullptr);
bool Set(const std::string& name, const std::string& value);
Snapshot List();
uint32_t AreaSerial();
// A null name waits for the area generation. A false return leaves new_serial
// untouched; a null timeout waits indefinitely. Locks are released while idle.
bool Wait(const std::string* name, uint32_t old_serial, uint32_t* new_serial,
          const WaitTimeout* timeout);

}  // namespace gesher::properties
