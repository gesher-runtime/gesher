// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Replace libbase's process-local host property map with Gesher's shared area.
#include <gesher/system_properties.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

struct prop_info { std::string name; };

extern "C" int __system_property_set(const char* name, const char* value) {
  return name && gesher::properties::Set(name, value ? value : "") ? 0 : -1;
}
extern "C" int __system_property_get(const char* name, char* value) {
  if (!value) return 0;
  std::string text;
  if (name) gesher::properties::Get(name, &text);
  size_t size = std::min(text.size(), size_t{91});
  std::memcpy(value, text.data(), size);
  value[size] = 0;
  return static_cast<int>(size);
}
extern "C" const prop_info* __system_property_find(const char* name) {
  if (!name || !gesher::properties::Get(name, nullptr)) return nullptr;
  static auto* mutex = new std::mutex;
  static auto* infos = new std::map<std::string, std::unique_ptr<prop_info>>;
  std::lock_guard lock(*mutex);
  auto& info = (*infos)[name];
  if (!info) info = std::make_unique<prop_info>(prop_info{name});
  return info.get();
}
extern "C" void __system_property_read_callback(const prop_info* info,
    void (*callback)(void*, const char*, const char*, uint32_t), void* cookie) {
  if (!info || !callback) return;
  std::string value;
  uint32_t serial = 0;
  gesher::properties::Get(info->name, &value, &serial);
  callback(cookie, info->name.c_str(), value.c_str(), serial);
}
