// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <gesher/logging.h>
#include <gesher/system_properties.h>
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>

namespace gesher::logging {
namespace {
constexpr DWORD kLimit = 16 * 1024 * 1024;
constexpr uint32_t kMagic = 0x474c4f47;
struct Header {
  uint32_t magic, size;
  uint64_t timestamp;
  uint32_t pid, tid;
  int32_t priority, buffer;
  uint32_t tag_size, text_size;
};
static_assert(sizeof(Header) == 40);
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Lock {
  HANDLE mutex;
  bool held;
  explicit Lock(HANDLE handle) : mutex(handle), held(false) {
    if (handle) {
      DWORD result = WaitForSingleObject(handle, INFINITE);
      held = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
  }
  ~Lock() { if (held) ReleaseMutex(mutex); }
};
struct File {
  Handle mutex, file;
  bool Open(std::string* error) {
    wchar_t environment[32768];
    DWORD length = GetEnvironmentVariableW(L"GESHER_LOG_FILE", environment, 32768);
    if (!length || length >= 32768) { if (error) *error = "GESHER_LOG_FILE is unset or invalid"; return false; }
    std::wstring path(environment, length);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    wchar_t absolute[32768];
    length = GetFullPathNameW(path.c_str(), 32768, absolute, nullptr);
    if (!length || length >= 32768) { if (error) *error = "invalid log path"; return false; }
    uint64_t hash = 14695981039346656037ULL;
    for (DWORD i = 0; i < length; ++i) {
      wchar_t c = absolute[i];
      if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
      hash = (hash ^ static_cast<uint16_t>(c)) * 1099511628211ULL;
    }
    wchar_t name[64];
    std::swprintf(name, 64, L"Local\\gesher-log-%016llx", static_cast<unsigned long long>(hash));
    mutex.value = CreateMutexW(nullptr, FALSE, name);
    file.value = CreateFileW(absolute, GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!mutex.value || file.value == INVALID_HANDLE_VALUE) {
      if (error) *error = "cannot open the structured log (Windows error " + std::to_string(GetLastError()) + ")";
      return false;
    }
    return true;
  }
};
bool Truncate(HANDLE file) {
  LARGE_INTEGER start{};
  return SetFilePointerEx(file, start, nullptr, FILE_BEGIN) && SetEndOfFile(file);
}
int PropertyPriority(const std::string& persistent, const std::string& transient) {
  std::string value;
  properties::Get(persistent, &value);
  if (value.empty()) properties::Get(transient, &value);
  if (value.empty()) return -1;
  if (_stricmp(value.c_str(), "false") == 0) return 8;
  // Android accepts the first priority letter, case-insensitively.
  return Priority(value.front());
}
}  // namespace

int Priority(char letter) {
  if (letter >= 'a' && letter <= 'z') letter -= 'a' - 'A';
  switch (letter) {
    case 'V': return 2; case 'D': return 3; case 'I': return 4;
    case 'W': return 5; case 'E': return 6; case 'F': case 'A': return 7;
    case 'S': return 8; default: return -1;
  }
}
char PriorityLetter(int priority) {
  constexpr char letters[] = "??VDIWEFS";
  return priority >= 0 && priority <= 8 ? letters[priority] : '?';
}
bool IsLoggable(int priority, const char* tag, size_t length, int fallback, int minimum) {
  int property = -1;
  if (tag && length) {
    std::string name(tag, length);
    property = PropertyPriority("persist.log.tag." + name, "log.tag." + name);
  }
  if (property < 0) property = PropertyPriority("persist.log.tag", "log.tag");
  int threshold = property >= 0 ? property : minimum != 0 ? minimum : fallback;
  if (property >= 0 && minimum != 0) threshold = std::min(property, minimum);
  return priority >= threshold;
}
void Record(int buffer, int priority, const char* tag, const char* text) {
  File file;
  if (!file.Open(nullptr)) return;
  Lock lock(file.mutex.value);
  if (!lock.held) return;
  Entry entry;
  FILETIME time;
  GetSystemTimePreciseAsFileTime(&time);
  entry.timestamp = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  entry.pid = GetCurrentProcessId();
  entry.tid = GetCurrentThreadId();
  entry.priority = priority;
  entry.buffer = buffer;
  entry.tag = tag ? std::string(tag).substr(0, 4096) : "";
  entry.text = text ? std::string(text).substr(0, 4096) : "";
  Header header{kMagic, static_cast<uint32_t>(sizeof(Header) + entry.tag.size() + entry.text.size()),
                entry.timestamp, entry.pid, entry.tid, priority, buffer,
                static_cast<uint32_t>(entry.tag.size()), static_cast<uint32_t>(entry.text.size())};
  std::string data(reinterpret_cast<const char*>(&header), sizeof(header));
  data += entry.tag;
  data += entry.text;
  LARGE_INTEGER size{}, end{};
  if (!GetFileSizeEx(file.file.value, &size)) return;
  if (size.QuadPart + static_cast<int64_t>(data.size()) > kLimit && !Truncate(file.file.value)) return;
  if (!SetFilePointerEx(file.file.value, end, nullptr, FILE_END)) return;
  DWORD written = 0;
  WriteFile(file.file.value, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
}
bool Read(std::vector<Entry>* entries, std::string* error) {
  File file;
  if (!file.Open(error)) return false;
  Lock lock(file.mutex.value);
  if (!lock.held) { if (error) *error = "cannot lock the log"; return false; }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.file.value, &size) || size.QuadPart > kLimit || size.QuadPart < 0) {
    if (error) *error = "invalid log size";
    return false;
  }
  std::string bytes(static_cast<size_t>(size.QuadPart), 0);
  DWORD count = 0;
  if (!ReadFile(file.file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size()) {
    if (error) *error = "cannot read the log";
    return false;
  }
  entries->clear();
  for (size_t at = 0; at < bytes.size();) {
    Header header{};
    if (bytes.size() - at < sizeof(header)) { if (error) *error = "incomplete log header"; return false; }
    std::memcpy(&header, bytes.data() + at, sizeof(header));
    if (header.magic != kMagic || header.tag_size > 4096 || header.text_size > 4096 ||
        header.size != sizeof(header) + header.tag_size + header.text_size || header.size > bytes.size() - at) {
      if (error) *error = "invalid log record";
      return false;
    }
    Entry entry{header.timestamp, header.pid, header.tid, header.priority, header.buffer, {}, {}};
    entry.tag.assign(bytes.data() + at + sizeof(header), header.tag_size);
    entry.text.assign(bytes.data() + at + sizeof(header) + header.tag_size, header.text_size);
    entries->push_back(std::move(entry));
    at += header.size;
  }
  return true;
}
bool Clear(std::string* error) {
  File file;
  if (!file.Open(error)) return false;
  Lock lock(file.mutex.value);
  if (!lock.held || !Truncate(file.file.value)) { if (error) *error = "cannot clear the log"; return false; }
  return true;
}
std::string Format(const Entry& entry, const std::string& format) {
  char prefix[160];
  char priority = PriorityLetter(entry.priority);
  std::string tag = entry.tag;
  if (tag.size() < 8) tag.append(8 - tag.size(), ' ');
  char pid[32];
  std::snprintf(pid, sizeof(pid), "%5u", entry.pid);
  FILETIME utc{static_cast<DWORD>(entry.timestamp), static_cast<DWORD>(entry.timestamp >> 32)}, local;
  SYSTEMTIME time{};
  if (FileTimeToLocalFileTime(&utc, &local)) FileTimeToSystemTime(&local, &time);
  char stamp[64];
  std::snprintf(stamp, sizeof(stamp), "%02u-%02u %02u:%02u:%02u.%03u", time.wMonth, time.wDay,
                 time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
  std::string result;
  if (format == "long") {
    std::snprintf(prefix, sizeof(prefix), "[ %s %5u:%5u %c/", stamp, entry.pid, entry.tid, priority);
    return std::string(prefix) + tag + " ]\n" + entry.text + "\n\n";
  }
  size_t at = 0;
  do {
    size_t end = entry.text.find('\n', at);
    std::string line = entry.text.substr(at, end == std::string::npos ? std::string::npos : end - at);
    if (format == "raw") result += line + '\n';
    else if (format == "tag") result += std::string(1, priority) + '/' + tag + ": " + line + '\n';
    else {
      if (format == "process") std::snprintf(prefix, sizeof(prefix), "%c(%5u) ", priority, entry.pid);
      else if (format == "thread") std::snprintf(prefix, sizeof(prefix), "%c(%5u:%5u) ", priority, entry.pid, entry.tid);
      else if (format == "threadtime") std::snprintf(prefix, sizeof(prefix), "%s %5u %5u %c ", stamp, entry.pid, entry.tid, priority);
      else if (format == "time") std::snprintf(prefix, sizeof(prefix), "%s %c/", stamp, priority);
      else if (format == "epoch") std::snprintf(prefix, sizeof(prefix), "%.3f %5u %5u %c ",
          (static_cast<double>(entry.timestamp) - 116444736000000000.0) / 10000000.0, entry.pid, entry.tid, priority);
      else std::snprintf(prefix, sizeof(prefix), "%c/", priority);
      result += prefix;
      if (format == "brief" || format == "time") result += tag + '(' + pid + "): ";
      else if (format == "threadtime" || format == "epoch") result += tag + ": ";
      result += line;
      if (format == "process") result += "  (" + entry.tag + ')';
      result += '\n';
    }
    if (end == std::string::npos || end + 1 == entry.text.size()) break;
    at = end + 1;
  } while (at <= entry.text.size());
  return result;
}
}  // namespace gesher::logging
