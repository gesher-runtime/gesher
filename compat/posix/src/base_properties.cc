// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Replace libbase's process-local host property map with Gesher's shared area.
#include <gesher/system_properties.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>

struct prop_info {
  std::string name;
  // android::base::CachedProperty retains the callback pointer for ro values.
  // These objects and their strings live until process exit.
  std::string immutable_value;
  uint32_t immutable_serial;
};

extern "C" int __system_property_set(const char* name, const char* value) {
  return name && gesher::properties::Set(name, value ? value : "") ? 0 : -1;
}
extern "C" int __system_property_get(const char* name, char* value) {
  if (!value) return 0;
  std::string text;
  if (name) gesher::properties::Get(name, &text);
  if (name && std::strncmp(name, "ro.", 3) == 0 && text.size() >= 92) text = gesher::properties::kLongLegacyError;
  size_t size = std::min(text.size(), size_t{91});
  std::memcpy(value, text.data(), size);
  value[size] = 0;
  return static_cast<int>(size);
}
extern "C" const prop_info* __system_property_find(const char* name) {
  std::string value;
  uint32_t serial = 0;
  if (!name || !gesher::properties::Get(name, &value, &serial)) return nullptr;
  static auto* mutex = new std::mutex;
  static auto* infos = new std::map<std::string, std::unique_ptr<prop_info>>;
  std::lock_guard lock(*mutex);
  auto& info = (*infos)[name];
  if (!info) info = std::make_unique<prop_info>(prop_info{name, std::move(value), serial});
  return info.get();
}
extern "C" void __system_property_read_callback(const prop_info* info,
    void (*callback)(void*, const char*, const char*, uint32_t), void* cookie) {
  if (!info || !callback) return;
  if (info->name.rfind("ro.", 0) == 0) {
    callback(cookie, info->name.c_str(), info->immutable_value.c_str(), info->immutable_serial);
    return;
  }
  std::string value;
  uint32_t serial = 0;
  gesher::properties::Get(info->name, &value, &serial);
  callback(cookie, info->name.c_str(), value.c_str(), serial);
}
extern "C" uint32_t __system_property_serial(const prop_info* info) {
  uint32_t serial = 0;
  if (info) gesher::properties::Get(info->name, nullptr, &serial);
  return serial;
}
extern "C" uint32_t __system_property_area_serial() { return gesher::properties::AreaSerial(); }
extern "C" int __system_property_read(const prop_info* info, char* name, char* value) {
  if (!info) return -1;
  if (name) {
    size_t size = std::min(info->name.size(), size_t{31});
    std::memcpy(name, info->name.data(), size);
    name[size] = 0;
  }
  char ignored[92];
  return __system_property_get(info->name.c_str(), value ? value : ignored);
}
extern "C" int __system_property_foreach(void (*callback)(const prop_info*, void*), void* cookie) {
  if (!callback) return -1;
  for (const auto& [name, value] : gesher::properties::List()) {
    callback(__system_property_find(name.c_str()), cookie);
  }
  return 0;
}
extern "C" bool __system_property_wait(const prop_info* info, uint32_t old_serial, uint32_t* new_serial,
                                      const timespec* timeout) {
  gesher::properties::WaitTimeout relative{};
  if (timeout) relative = {timeout->tv_sec, timeout->tv_nsec};
  return gesher::properties::Wait(info ? &info->name : nullptr, old_serial, new_serial, timeout ? &relative : nullptr);
}
extern "C" uint32_t __system_property_wait_any(uint32_t old_serial) {
  uint32_t serial = old_serial;
  gesher::properties::Wait(nullptr, old_serial, &serial, nullptr);
  return serial;
}
