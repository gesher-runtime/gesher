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
#include <memory>

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
// GESHER_LOG_FILE, with '/' as '\\'.
bool LogVariable(std::wstring* value) {
  wchar_t buffer[1024];
  DWORD length = GetEnvironmentVariableW(L"GESHER_LOG_FILE", buffer, 1024);
  if (length && length < 1024) {
    value->assign(buffer, length);
  } else if (length >= 1024 && length < 32768) {
    value->resize(length);
    length = GetEnvironmentVariableW(L"GESHER_LOG_FILE", value->data(), length);
    if (!length || length >= value->size()) return false;
    value->resize(length);
  } else {
    return false;
  }
  std::replace(value->begin(), value->end(), L'/', L'\\');
  return true;
}
// Whether GetFullPathNameW's result for |path| is independent of the current
// directory (and so may be cached with the variable's value).
bool Absolute(const std::wstring& path) {
  return (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') ||
         (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\');
}
bool FullPath(const std::wstring& path, std::wstring* absolute) {
  wchar_t full[32768];
  DWORD length = GetFullPathNameW(path.c_str(), 32768, full, nullptr);
  if (!length || length >= 32768) return false;
  absolute->assign(full, length);
  return true;
}
struct File {
  Handle mutex, file;
  bool Open(std::string* error) {
    std::wstring variable, absolute;
    if (!LogVariable(&variable)) { if (error) *error = "GESHER_LOG_FILE is unset or invalid"; return false; }
    if (!FullPath(variable, &absolute)) { if (error) *error = "invalid log path"; return false; }
    return Open(absolute, error);
  }
  bool Open(const std::wstring& absolute, std::string* error) {
    uint64_t hash = 14695981039346656037ULL;
    for (wchar_t c : absolute) {
      if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
      hash = (hash ^ static_cast<uint16_t>(c)) * 1099511628211ULL;
    }
    wchar_t name[64];
    std::swprintf(name, 64, L"Local\\gesher-log-%016llx", static_cast<unsigned long long>(hash));
    mutex.value = CreateMutexW(nullptr, FALSE, name);
    // FILE_SHARE_DELETE: a process keeps its log open (Record), and the log
    // must stay deletable and renameable meanwhile (Current notices).
    file.value = CreateFileW(absolute.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!mutex.value || file.value == INVALID_HANDLE_VALUE) {
      if (error) *error = "cannot open the structured log (Windows error " + std::to_string(GetLastError()) + ")";
      return false;
    }
    return Name(file.value, &name_at_open);
  }
  // The file's name on its volume now (renames change it; a POSIX delete
  // moves it under \$Extend\$Deleted).
  static bool Name(HANDLE handle, std::wstring* name) {
    alignas(FILE_NAME_INFO) char buffer[sizeof(FILE_NAME_INFO) + 2048 * sizeof(wchar_t)];
    auto* info = reinterpret_cast<FILE_NAME_INFO*>(buffer);
    if (!GetFileInformationByHandleEx(handle, FileNameInfo, info, sizeof(buffer))) return false;
    name->assign(info->FileName, info->FileNameLength / sizeof(wchar_t));
    return true;
  }
  // Whether the open file is still the one at the log's path: not deleted
  // (or about to be) and not renamed away. Called under the log's mutex.
  bool Current() const {
    FILE_STANDARD_INFO standard;
    if (!GetFileInformationByHandleEx(file.value, FileStandardInfo, &standard, sizeof(standard)) ||
        standard.DeletePending) return false;
    std::wstring now;
    return Name(file.value, &now) && now == name_at_open;
  }
  std::wstring name_at_open;
};
// Record runs for every log line of every thread, so each process keeps its
// log's mutex and file open instead of opening both per line. The
// cross-process guarantees are unchanged: every writer and reader still takes
// the named mutex, and under it a writer reads the file's current size (other
// processes append to and truncate the same file) and writes at that offset.
// Changing GESHER_LOG_FILE (Main sets it in-process) reopens, and so does the
// file being deleted or renamed away (File::Current). The cache's lock is only
// held to copy or replace the shared File, never across the named mutex's
// wait, so a thread that dies while logging (at process exit, in DLL detach)
// cannot block the others: the mutex is then abandoned, which Lock accepts.
// The cache itself is never destroyed: logging continues through static
// destruction.
struct Cache {
  SRWLOCK lock = SRWLOCK_INIT;
  std::wstring variable, path;  // GESHER_LOG_FILE and its absolute path
  std::shared_ptr<File> file;
};
Cache& Cached() {
  static Cache* cache = new Cache;
  return *cache;
}
bool Truncate(HANDLE file) {
  LARGE_INTEGER start{};
  return SetFilePointerEx(file, start, nullptr, FILE_BEGIN) && SetEndOfFile(file);
}
// The priority a persistent property, or else its transient one, sets.
int PropertyPriority(const std::string& persistent, const std::string& transient) {
  const std::string& value = persistent.empty() ? transient : persistent;
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
  // One property lookup per log line, for the tag's and the global settings.
  std::string names[4] = {"persist.log.tag", "log.tag"}, values[4];
  size_t count = 2;
  if (tag && length) {
    std::string name(tag, length);
    names[2] = "persist.log.tag." + name;
    names[3] = "log.tag." + name;
    count = 4;
  }
  properties::GetAll(names, values, count);
  int property = count == 4 ? PropertyPriority(values[2], values[3]) : -1;
  if (property < 0) property = PropertyPriority(values[0], values[1]);
  constexpr int kAndroidLogDefault = 1;  // android_LogPriority's default sentinel
  int threshold = property >= 0 ? property : minimum != kAndroidLogDefault ? minimum : fallback;
  if (property >= 0 && minimum != kAndroidLogDefault) threshold = std::min(property, minimum);
  return priority >= threshold;
}
void Record(int buffer, int priority, const char* tag, const char* text) {
  std::wstring variable;
  if (!LogVariable(&variable)) return;
  size_t tag_size = tag ? strnlen(tag, 4096) : 0, text_size = text ? strnlen(text, 4096) : 0;
  Header header{kMagic, static_cast<uint32_t>(sizeof(Header) + tag_size + text_size), 0,
                GetCurrentProcessId(), GetCurrentThreadId(), priority, buffer,
                static_cast<uint32_t>(tag_size), static_cast<uint32_t>(text_size)};
  std::string data;
  data.reserve(header.size);
  data.append(reinterpret_cast<const char*>(&header), sizeof(header));
  if (tag_size) data.append(tag, tag_size);
  if (text_size) data.append(text, text_size);
  Cache& cache = Cached();
  // At most two attempts: the cached file, and a fresh one if it went stale.
  for (int attempt = 0; attempt < 2; ++attempt) {
    std::shared_ptr<File> file;
    AcquireSRWLockShared(&cache.lock);
    if (cache.file && cache.variable == variable && Absolute(variable)) file = cache.file;
    ReleaseSRWLockShared(&cache.lock);
    if (!file) {
      std::wstring path;
      if (!FullPath(variable, &path)) return;
      AcquireSRWLockExclusive(&cache.lock);
      if (cache.file && cache.path == path) {
        file = cache.file;
        cache.variable = variable;
      }
      ReleaseSRWLockExclusive(&cache.lock);
      if (!file) {
        file = std::make_shared<File>();
        if (!file->Open(path, nullptr)) return;
        AcquireSRWLockExclusive(&cache.lock);
        cache.file = file;
        cache.path = std::move(path);
        cache.variable = variable;
        ReleaseSRWLockExclusive(&cache.lock);
      }
    }
    HANDLE handle = file->file.value;
    Lock lock(file->mutex.value);
    if (!lock.held) return;
    if (!file->Current()) {
      // Deleted or renamed away: forget it (its handles close with the last
      // user) and write to the file now at the path.
      AcquireSRWLockExclusive(&cache.lock);
      if (cache.file == file) cache.file.reset();
      ReleaseSRWLockExclusive(&cache.lock);
      continue;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) return;
    bool fits = size.QuadPart + static_cast<int64_t>(data.size()) <= kLimit;
    if (!fits && Truncate(handle)) size.QuadPart = 0;
    if (fits || size.QuadPart == 0) {
      // Stamped under the lock, so the file's records stay in time order.
      FILETIME time;
      GetSystemTimePreciseAsFileTime(&time);
      header.timestamp = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
      std::memcpy(data.data(), &header, sizeof(header));
      OVERLAPPED at{};
      at.Offset = static_cast<DWORD>(size.QuadPart);
      at.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(size.QuadPart) >> 32);
      DWORD written = 0;
      WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &written, &at);
    }
    return;
  }
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
