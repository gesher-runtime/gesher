// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// The property area used by Java/libbase, ELF bionic and getprop/setprop.
// Device boot names isolate volatile properties; persist.* survive a new boot.

#ifdef GESHER_PROPERTY_DIAGNOSTIC
#include "../include/gesher/system_properties.h"
#else
#include <gesher/system_properties.h>
#endif

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace gesher::properties {
namespace {

constexpr DWORD kCapacity = 4 * 1024 * 1024;
constexpr uint32_t kMagic = 0x47535052; // GSPR
constexpr uint32_t kVersion = 2;
uint32_t Serial(const std::string& name, const std::string& text, uint32_t previous = 0,
                bool update = false) {
  if (name.rfind("ro.", 0) == 0 && text.size() >= 92) {
    return (static_cast<uint32_t>(sizeof(kLongLegacyError) - 1) << 24) | (1U << 16);
  }
  return (static_cast<uint32_t>(text.size()) << 24) |
         (update ? (previous + 2) & 0x00fffffeU : 0);
}
struct Header {
  uint32_t magic;
  uint32_t version;
  uint32_t generation;
  uint32_t used;
};

struct Handle {
  HANDLE value = nullptr;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
#ifdef GESHER_PROPERTY_READONLY_DIAGNOSTIC
thread_local HANDLE diagnostic_wait_event = nullptr;
HANDLE diagnostic_inherited_mutex = nullptr;
#endif
struct MutexLock {
  HANDLE mutex;
  bool held = false;
  explicit MutexLock(HANDLE handle) : mutex(handle) {
    if (mutex) {
#ifdef GESHER_PROPERTY_READONLY_DIAGNOSTIC
      if (diagnostic_wait_event) SetEvent(diagnostic_wait_event);
#endif
      DWORD result = WaitForSingleObject(mutex, INFINITE);
      held = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
  }
  ~MutexLock() { if (held) ReleaseMutex(mutex); }
};
struct Area {
  Handle mutex, file, mapping;
  void* view = nullptr;
#ifdef GESHER_PROPERTY_READONLY_DIAGNOSTIC
  bool readonly = false;
#endif
  uint32_t cached_generation = 0;
  Snapshot values;
  std::wstring root, persistent;
  ~Area() { if (view) UnmapViewOfFile(view); }
};
bool Valid(const std::string& name, const std::string& value);

// Deliberately leaked: property callbacks can run during static destruction.
std::mutex& LocalMutex() { static auto* mutex = new std::mutex; return *mutex; }
std::map<std::wstring, std::unique_ptr<Area>>& Areas() {
  static auto* areas = new std::map<std::wstring, std::unique_ptr<Area>>;
  return *areas;
}

std::wstring Environment(const wchar_t* name) {
  DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
  if (!size) return {};
  std::wstring result(size, 0);
  DWORD count = GetEnvironmentVariableW(name, result.data(), size);
  if (!count || count >= size) return {};
  result.resize(count);
  return result;
}
std::wstring FullPath(std::wstring path) {
  std::replace(path.begin(), path.end(), L'/', L'\\');
  DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (!size) return {};
  std::wstring result(size, 0);
  DWORD count = GetFullPathNameW(path.c_str(), size, result.data(), nullptr);
  if (!count || count >= size) return {};
  result.resize(count);
  return result;
}
void Diagnostic(const char* stage, DWORD error) {
#ifdef GESHER_PROPERTY_DIAGNOSTIC
  std::fprintf(stderr, "PROPERTY_DIAGNOSTIC stage=%s error=%lu\n", stage, error);
#else
  (void)stage; (void)error;
#endif
}
bool Directories(const std::wstring& path) {
  for (size_t i = 3; i <= path.size(); ++i) {
    if (i != path.size() && path[i] != L'\\') continue;
    std::wstring part = path.substr(0, i);
    if (!CreateDirectoryW(part.c_str(), nullptr)) {
      DWORD error = GetLastError();
      if (error != ERROR_ALREADY_EXISTS) { Diagnostic("directory", error); return false; }
    }
  }
  return true;
}
std::wstring Key(const std::wstring& path) {
  uint64_t hash = 14695981039346656037ULL;
  for (wchar_t ch : path) {
    if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
    hash = (hash ^ static_cast<uint16_t>(ch)) * 1099511628211ULL;
  }
  wchar_t text[24];
  std::swprintf(text, sizeof(text) / sizeof(text[0]), L"%016llx", static_cast<unsigned long long>(hash));
  return text;
}
std::wstring Session() {
  std::wstring session = Environment(L"GESHER_BINDER");
  if (!session.empty()) return L"device-" + session;
  session = Environment(L"GESHER_PROPERTY_SESSION");
  if (session.empty()) {
    FILETIME time;
    GetSystemTimePreciseAsFileTime(&time);
    uint64_t ticks = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    session = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(ticks);
    if (_wputenv_s(L"GESHER_PROPERTY_SESSION", session.c_str()) != 0) return {};
  }
  return L"single-" + session;
}
bool InheritRoot(const std::wstring& session, const std::wstring& root) {
  if (Environment(L"GESHER_PROPERTY_ROOT_SESSION") == session &&
      Environment(L"GESHER_PROPERTY_ROOT") == root) return true;
  return _wputenv_s(L"GESHER_PROPERTY_ROOT", root.c_str()) == 0 &&
         _wputenv_s(L"GESHER_PROPERTY_ROOT_SESSION", session.c_str()) == 0;
}

void Put32(std::string* data, uint32_t value) {
  data->append(reinterpret_cast<const char*>(&value), sizeof(value));
}
bool Take32(const std::string& data, size_t* at, uint32_t* value) {
  if (data.size() - *at < sizeof(*value)) return false;
  std::memcpy(value, data.data() + *at, sizeof(*value));
  *at += sizeof(*value);
  return true;
}
std::string Encode(const Snapshot& values) {
  std::string data;
  Put32(&data, static_cast<uint32_t>(values.size()));
  for (const auto& [name, value] : values) {
    Put32(&data, static_cast<uint32_t>(name.size()));
    Put32(&data, static_cast<uint32_t>(value.text.size()));
    Put32(&data, value.serial);
    data += name;
    data += value.text;
  }
  return data;
}
bool Decode(const std::string& data, Snapshot* values) {
  size_t at = 0;
  uint32_t count;
  if (!Take32(data, &at, &count) || count > kCapacity / 12) return false;
  Snapshot parsed;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t name_size, value_size, serial;
    if (!Take32(data, &at, &name_size) || !Take32(data, &at, &value_size) ||
        !Take32(data, &at, &serial) || name_size > data.size() - at) return false;
    std::string name = data.substr(at, name_size);
    at += name_size;
    if (value_size > data.size() - at) return false;
    parsed.emplace(std::move(name), Value{data.substr(at, value_size), serial});
    at += value_size;
  }
  if (at != data.size()) return false;
  *values = std::move(parsed);
  return true;
}
bool Read(const std::wstring& path, std::string* data, bool optional) {
  Handle file;
  file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file.value == INVALID_HANDLE_VALUE) return optional && GetLastError() == ERROR_FILE_NOT_FOUND;
  LARGE_INTEGER size;
  if (!GetFileSizeEx(file.value, &size) || size.QuadPart > kCapacity || size.QuadPart < 0) return false;
  data->resize(static_cast<size_t>(size.QuadPart));
  DWORD count = 0;
  return ReadFile(file.value, data->data(), static_cast<DWORD>(data->size()), &count, nullptr) &&
         count == data->size();
}
bool WritePersistent(const std::wstring& path, const Snapshot& values) {
  std::string data = Encode(values);
  if (data.size() > kCapacity) return false;
  std::wstring temp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
  Handle file;
  file.value = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file.value == INVALID_HANDLE_VALUE) return false;
  DWORD count = 0;
  bool ok = WriteFile(file.value, data.data(), static_cast<DWORD>(data.size()), &count, nullptr) &&
            count == data.size() && FlushFileBuffers(file.value);
  CloseHandle(file.value);
  file.value = nullptr;
  if (ok) ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!ok) DeleteFileW(temp.c_str());
  return ok;
}
bool Persist(const std::wstring& path, const std::string* name, const Value* value, Snapshot* initial) {
  Handle mutex;
  mutex.value = CreateMutexW(nullptr, FALSE, (L"Local\\gesher-properties-persist-" + Key(path)).c_str());
  MutexLock lock(mutex.value);
  if (!lock.held) return false;
  std::string data;
  Snapshot values;
  if (!Read(path, &data, true) || (!data.empty() && !Decode(data, &values))) return false;
  if (initial) {
    for (const auto& [key, entry] : values) {
      if (key.rfind("persist.", 0) == 0 && Valid(key, entry.text)) (*initial)[key] = entry;
    }
  }
  if (!name) return true;
  values[*name] = *value;
  return WritePersistent(path, values);
}

std::string Trim(const std::string& text) {
  size_t start = text.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return {};
  return text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
}
Snapshot Initial() {
  Snapshot values;
  for (const auto& pair : {
         std::pair{"ro.build.version.sdk", "36"}, {"ro.build.version.release", "16"},
         {"ro.build.version.codename", "REL"}, {"ro.product.cpu.abi", "x86_64"},
         {"ro.product.cpu.abilist", "x86_64,arm64-v8a"}, {"ro.product.cpu.abilist64", "x86_64,arm64-v8a"},
         {"ro.product.manufacturer", "gesher"}, {"ro.product.model", "Gesher"},
         {"ro.product.brand", "gesher"}, {"ro.hardware", "gesher"}, {"ro.debuggable", "0"}}) {
    values[pair.first] = {pair.second, 1};
  }
  std::wstring root = Environment(L"ANDROID_ROOT");
  if (root.empty()) return values;
  std::string data;
  if (!Read(FullPath(root + L"/build.prop"), &data, true)) return values;
  size_t at = 0;
  while (at < data.size()) {
    size_t end = data.find('\n', at);
    std::string line = Trim(data.substr(at, end == std::string::npos ? std::string::npos : end - at));
    at = end == std::string::npos ? data.size() : end + 1;
    if (line.empty() || line[0] == '#') continue;
    size_t equal = line.find('=');
    if (equal == std::string::npos || equal == 0) continue;
    std::string key = Trim(line.substr(0, equal));
    bool optional = !key.empty() && key.back() == '?';
    if (optional) key = Trim(key.substr(0, key.size() - 1));
    std::string value = Trim(line.substr(equal + 1));
    if (Valid(key, value) && (!optional || !values.count(key))) values[key] = {std::move(value), 1};
  }
  return values;
}
bool Refresh(Area* area) {
  const auto* header = static_cast<Header*>(area->view);
  if (header->magic != kMagic || header->version != kVersion || header->used > kCapacity - sizeof(Header)) return false;
  if (header->generation == area->cached_generation) return true;
  std::string data(static_cast<const char*>(area->view) + sizeof(Header), header->used);
  if (!Decode(data, &area->values)) return false;
  area->cached_generation = header->generation;
  return true;
}
bool Publish(Area* area, Snapshot values) {
  std::string data = Encode(values);
  if (data.size() > kCapacity - sizeof(Header)) return false;
  auto* header = static_cast<Header*>(area->view);
  uint32_t generation = header->generation + 1;
  if (!generation) generation = 1;
  std::memcpy(static_cast<char*>(area->view) + sizeof(Header), data.data(), data.size());
  *header = {kMagic, kVersion, generation, static_cast<uint32_t>(data.size())};
  area->values = std::move(values);
  area->cached_generation = generation;
  return true;
}
void RemoveOldAreas(const std::wstring& directory, const std::wstring& current) {
  // Open areas deny FILE_SHARE_DELETE. Stale boot files can be removed, while
  // another live device's mapping stays usable. Only our exact generated
  // filename shape is eligible; persistent_properties is in another directory.
  WIN32_FIND_DATAW entry;
  HANDLE find = FindFirstFileW((directory + L"\\*.area").c_str(), &entry);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    std::wstring name = entry.cFileName;
    if (name.size() != 21 || name.substr(16) != L".area") continue;
    bool generated = true;
    for (size_t i = 0; i < 16; ++i) {
      if (!((name[i] >= L'0' && name[i] <= L'9') || (name[i] >= L'a' && name[i] <= L'f'))) generated = false;
    }
    std::wstring path = directory + L"\\" + name;
    if (generated && path != current) DeleteFileW(path.c_str());
  } while (FindNextFileW(find, &entry));
  FindClose(find);
}
Area* Current() { // LocalMutex is held by every caller.
  std::wstring session = Session();
  if (session.empty()) return nullptr;
  auto& cached = Areas();
  auto existing = cached.find(session);
  if (existing != cached.end()) {
    Area* area = existing->second.get();
    return InheritRoot(session, area->root) ? area : nullptr;
  }
  // A boot's mapping keeps its original data root, as Android's property area
  // does. ART tests temporarily replace ANDROID_DATA for scratch directories;
  // ordinary log/property reads must not create a new service in each scratch.
  // Children must inherit this same boot root even when ART has temporarily
  // changed ANDROID_DATA before spawning them. A different session starts a
  // new boot and deliberately ignores the previous root's inherited binding.
  std::wstring data = Environment(L"GESHER_PROPERTY_ROOT_SESSION") == session
                          ? Environment(L"GESHER_PROPERTY_ROOT") : std::wstring{};
  if (data.empty()) data = Environment(L"ANDROID_DATA");
  if (data.empty()) {
    data = Environment(L"LOCALAPPDATA");
    if (data.empty()) data = Environment(L"TEMP");
    if (data.empty()) return nullptr;
    data += L"\\Gesher\\data";
  }
  data = FullPath(data);
  if (data.empty()) return nullptr;
  std::wstring dir = data + L"\\system\\gesher-properties";
  std::wstring path = dir + L"\\" + Key(session) + L".area";
  if (!Directories(dir) || !Directories(data + L"\\property")) return nullptr;
  auto area = std::make_unique<Area>();
  area->root = data;
  area->persistent = data + L"\\property\\persistent_properties";
#ifdef GESHER_PROPERTY_READONLY_DIAGNOSTIC
  if (diagnostic_inherited_mutex) {
    if (!DuplicateHandle(GetCurrentProcess(), diagnostic_inherited_mutex, GetCurrentProcess(), &area->mutex.value,
                         SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, 0)) return nullptr;
  } else
#endif
  area->mutex.value = CreateMutexW(nullptr, FALSE, (L"Local\\gesher-properties-" + Key(path)).c_str());
  MutexLock lock(area->mutex.value);
  if (!lock.held) { Diagnostic("mutex", GetLastError()); return nullptr; }
  area->file.value = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
#ifdef GESHER_PROPERTY_READONLY_DIAGNOSTIC
  if (area->file.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
    area->file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    area->readonly = area->file.value != INVALID_HANDLE_VALUE;
  }
#endif
  if (area->file.value == INVALID_HANDLE_VALUE) { Diagnostic("file", GetLastError()); return nullptr; }
#ifdef GESHER_PROPERTY_READONLY_DIAGNOSTIC
  area->mapping.value = CreateFileMappingW(area->file.value, nullptr, area->readonly ? PAGE_READONLY : PAGE_READWRITE, 0, kCapacity, nullptr);
#else
  area->mapping.value = CreateFileMappingW(area->file.value, nullptr, PAGE_READWRITE, 0, kCapacity, nullptr);
#endif
  if (!area->mapping.value) { Diagnostic("mapping", GetLastError()); return nullptr; }
#ifdef GESHER_PROPERTY_READONLY_DIAGNOSTIC
  area->view = MapViewOfFile(area->mapping.value, area->readonly ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS, 0, 0, kCapacity);
#else
  area->view = MapViewOfFile(area->mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, kCapacity);
#endif
  if (!area->view) { Diagnostic("view", GetLastError()); return nullptr; }
  auto* header = static_cast<Header*>(area->view);
  if (!header->magic) {
#ifdef GESHER_PROPERTY_READONLY_DIAGNOSTIC
    if (area->readonly) return nullptr;
#endif
    Snapshot initial = Initial();
    if (!Persist(area->persistent, nullptr, nullptr, &initial)) return nullptr;
    for (auto& [name, value] : initial) value.serial = Serial(name, value.text);
    if (!Publish(area.get(), std::move(initial))) return nullptr;
  }
  if (!Refresh(area.get()) || !InheritRoot(session, data)) return nullptr;
  Area* result = area.get();
  cached.emplace(session, std::move(area));
  RemoveOldAreas(dir, path);
  return result;
}
bool Utf8(const std::string& value) {
  for (size_t at = 0; at < value.size();) {
    unsigned char lead = static_cast<unsigned char>(value[at++]);
    if (lead < 0x80) continue;
    unsigned trailing;
    uint32_t code, minimum;
    if (lead >= 0xc2 && lead <= 0xdf) { trailing = 1; code = lead & 0x1f; minimum = 0x80; }
    else if (lead >= 0xe0 && lead <= 0xef) { trailing = 2; code = lead & 0x0f; minimum = 0x800; }
    else if (lead >= 0xf0 && lead <= 0xf4) { trailing = 3; code = lead & 7; minimum = 0x10000; }
    else return false;
    if (trailing > value.size() - at) return false;
    while (trailing--) {
      unsigned char next = static_cast<unsigned char>(value[at++]);
      if ((next & 0xc0) != 0x80) return false;
      code = (code << 6) | (next & 0x3f);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
  }
  return true;
}
bool Valid(const std::string& name, const std::string& value) {
  if (name.empty() || name.size() > 255 || name.front() == '.' || name.back() == '.' ||
      name.find("..") != std::string::npos || value.find('\0') != std::string::npos || !Utf8(value)) return false;
  for (unsigned char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '.' || c == '_' || c == '-' || c == '@' || c == ':')) return false;
  }
  return value.size() < (name.rfind("ro.", 0) == 0 ? 65536U : 92U);
}

} // namespace

#ifdef GESHER_PROPERTY_READONLY_DIAGNOSTIC
// CI-only synchronization observation. The caller must release on its same thread.
void DiagnosticSetWaitEvent(HANDLE event) { diagnostic_wait_event = event; }
void DiagnosticSetInheritedMutex(HANDLE mutex) { diagnostic_inherited_mutex = mutex; }
HANDLE DiagnosticAreaMutex() {
  std::lock_guard local(LocalMutex());
  Area* area = Current();
  return area ? area->mutex.value : nullptr;
}
HANDLE DiagnosticAreaMapping() {
  std::lock_guard local(LocalMutex());
  Area* area = Current();
  return area ? area->mapping.value : nullptr;
}
HANDLE DiagnosticHoldAreaMutex() {
  std::lock_guard local(LocalMutex());
  Area* area = Current();
  if (!area) return nullptr;
  const DWORD result = WaitForSingleObject(area->mutex.value, 5000);
  return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED ? area->mutex.value : nullptr;
}
bool DiagnosticReleaseAreaMutex(HANDLE mutex) {
  return mutex && ReleaseMutex(mutex) != FALSE;
}
#endif
bool Initialize() {
  std::lock_guard lock(LocalMutex());
  return Current() != nullptr;
}
bool Get(const std::string& name, std::string* value, uint32_t* serial) {
  std::lock_guard local(LocalMutex());
  Area* area = Current();
  if (!area) return false;
  MutexLock lock(area->mutex.value);
  if (!lock.held || !Refresh(area)) return false;
  auto found = area->values.find(name);
  if (found == area->values.end()) return false;
  if (value) *value = found->second.text;
  if (serial) *serial = found->second.serial;
  return true;
}
bool Set(const std::string& name, const std::string& value) {
  if (!Valid(name, value)) return false;
  std::lock_guard local(LocalMutex());
  Area* area = Current();
  if (!area) return false;
#ifdef GESHER_PROPERTY_READONLY_DIAGNOSTIC
  if (area->readonly) return false;
#endif
  MutexLock lock(area->mutex.value);
  if (!lock.held || !Refresh(area)) return false;
  auto found = area->values.find(name);
  if (name.rfind("ro.", 0) == 0 && found != area->values.end()) return false;
  Value next{value, Serial(name, value, found == area->values.end() ? 0 : found->second.serial,
                           found != area->values.end())};
  Snapshot values = area->values;
  values[name] = next;
  if (Encode(values).size() > kCapacity - sizeof(Header)) return false;
  if (name.rfind("persist.", 0) == 0 && !Persist(area->persistent, &name, &next, nullptr)) return false;
  return Publish(area, std::move(values));
}
Snapshot List() {
  std::lock_guard local(LocalMutex());
  Area* area = Current();
  if (!area) return {};
  MutexLock lock(area->mutex.value);
  return lock.held && Refresh(area) ? area->values : Snapshot{};
}
uint32_t AreaSerial() {
  std::lock_guard local(LocalMutex());
  Area* area = Current();
  if (!area) return 0;
  MutexLock lock(area->mutex.value);
  return lock.held && Refresh(area) ? area->cached_generation : 0;
}
bool Wait(const std::string* name, uint32_t old_serial, uint32_t* new_serial,
          const WaitTimeout* timeout) {
  using Clock = std::chrono::steady_clock;
  if (timeout && (timeout->seconds < 0 || timeout->nanoseconds < 0 || timeout->nanoseconds >= 1000000000)) {
    return false;
  }
  auto deadline = Clock::time_point::max();
  if (timeout) {
    auto now = Clock::now();
    auto maximum = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
    int64_t requested = maximum;
    if (timeout->seconds <= (maximum - timeout->nanoseconds) / 1000000000) {
      requested = timeout->seconds * 1000000000 + timeout->nanoseconds;
    }
    deadline = now + std::chrono::nanoseconds(requested);
  }
  Area* area;
  {
    std::lock_guard local(LocalMutex());
    area = Current();
  }
  if (!area) return false;
  for (;;) {
    {
      std::lock_guard local(LocalMutex());
      MutexLock lock(area->mutex.value);
      if (!lock.held || !Refresh(area)) return false;
      uint32_t current = area->cached_generation;
      if (name) {
        auto found = area->values.find(*name);
        if (found == area->values.end()) return false;
        current = found->second.serial;
      }
      if (current != old_serial) {
        if (new_serial) *new_serial = current;
        return true;
      }
    }
    auto now = Clock::now();
    if (now >= deadline) return false;
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    Sleep(static_cast<DWORD>(std::min<int64_t>(10, std::max<int64_t>(1, remaining))));
  }
}

} // namespace gesher::properties
