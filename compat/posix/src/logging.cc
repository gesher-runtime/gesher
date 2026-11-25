// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <gesher/logging.h>
#include <gesher/system_properties.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
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
// Only the named mutex is retained between operations. A cached data handle
// would keep an ordinary DeleteFileW pending until every idle writer closed
// its copy, preventing recreation of the configured path.
struct Destination {
  Handle mutex;
  std::wstring path;
  bool Open(const std::wstring& absolute, std::string* error) {
    uint64_t hash = 14695981039346656037ULL;
    for (wchar_t c : absolute) {
      if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
      hash = (hash ^ static_cast<uint16_t>(c)) * 1099511628211ULL;
    }
    wchar_t name[64];
    std::swprintf(name, 64, L"Local\\gesher-log-%016llx", static_cast<unsigned long long>(hash));
    mutex.value = CreateMutexW(nullptr, FALSE, name);
    if (!mutex.value) {
      if (error) *error = "cannot open the log mutex (Windows error " + std::to_string(GetLastError()) + ")";
      return false;
    }
    path = absolute;
    return true;
  }
};
struct Cache {
  SRWLOCK lock = SRWLOCK_INIT;
  std::wstring variable;
  std::shared_ptr<Destination> destination;
};
// Deliberately leaked: logging continues through static destruction.
Cache& Cached() {
  static Cache* cache = new Cache;
  return *cache;
}
std::shared_ptr<Destination> CurrentDestination(std::string* error) {
  // Common absolute destinations need no temporary string after warmup.
  // Relative paths and long environment values retain FullPath's semantics.
  wchar_t configured[1024];
  DWORD length = GetEnvironmentVariableW(L"GESHER_LOG_FILE", configured, 1024);
  if (length && length < 1024) {
    Cache& cache = Cached();
    std::shared_ptr<Destination> found;
    AcquireSRWLockShared(&cache.lock);
    if (cache.destination && cache.variable.size() == length && Absolute(cache.variable) &&
        std::equal(configured, configured + length, cache.variable.begin(),
                   [](wchar_t a, wchar_t b) { return (a == L'/' ? L'\\' : a) == b; })) {
      found = cache.destination;
    }
    ReleaseSRWLockShared(&cache.lock);
    if (found) return found;
  }
  std::wstring variable;
  if (!LogVariable(&variable)) {
    if (error) *error = "GESHER_LOG_FILE is unset or invalid";
    return {};
  }
  Cache& cache = Cached();
  std::shared_ptr<Destination> destination;
  AcquireSRWLockShared(&cache.lock);
  if (cache.destination && cache.variable == variable && Absolute(variable)) {
    destination = cache.destination;
  }
  ReleaseSRWLockShared(&cache.lock);
  if (destination) return destination;
  std::wstring path;
  if (!FullPath(variable, &path)) {
    if (error) *error = "invalid log path";
    return {};
  }
  AcquireSRWLockShared(&cache.lock);
  if (cache.destination && cache.destination->path == path) destination = cache.destination;
  ReleaseSRWLockShared(&cache.lock);
  if (!destination) {
    destination = std::make_shared<Destination>();
    if (!destination->Open(path, error)) return {};
  }
  AcquireSRWLockExclusive(&cache.lock);
  cache.destination = destination;
  cache.variable = variable;
  ReleaseSRWLockExclusive(&cache.lock);
  return destination;
}
// Called only after acquiring the destination's named mutex. Denying delete
// sharing pins this operation to the opened object until its handle closes;
// rotation/replacement can then proceed between operations, including when
// other processes are idle. No cache lock is held across a mutex wait.
bool OpenFile(const Destination& destination, Handle* file, std::string* error) {
  file->value = CreateFileW(destination.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file->value == INVALID_HANDLE_VALUE) {
    if (error) *error = "cannot open the structured log (Windows error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  return true;
}
bool Resize(HANDLE file, int64_t length) {
  LARGE_INTEGER end;
  end.QuadPart = length;
  return SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
}
bool Truncate(HANDLE file) { return Resize(file, 0); }
// The priority a persistent property, or else its transient one, sets.
int PropertyPriority(const std::string& persistent, const std::string& transient) {
  const std::string& value = persistent.empty() ? transient : persistent;
  if (value.empty()) return -1;
  if (_stricmp(value.c_str(), "false") == 0) return 8;
  // Android accepts the first priority letter, case-insensitively.
  return Priority(value.front());
}
// Fixed process-wide storage, never one retained buffer per thread/tag. Longer
// explicit tags use the uncached lookup and retain the same property semantics.
constexpr size_t kPrioritySlots = 64, kCachedTagLength = 247;
struct PriorityEntry {
  properties::Revision revision;
  size_t length = 0;
  int priority = -1;
  std::array<char, kCachedTagLength> tag{};
};
struct PriorityCache {
  SRWLOCK lock = SRWLOCK_INIT;
  std::array<PriorityEntry, kPrioritySlots> entries;
};
static_assert(sizeof(PriorityCache) <= 20 * 1024);
PriorityCache& Priorities() {
  // Like the destination, the bounded cache remains usable in destructors.
  static auto* cache = new PriorityCache;
  return *cache;
}
int TagPriority(const char* tag, size_t length) {
  if (!tag) length = 0;
  properties::Revision revision = properties::CurrentRevision();
  bool cacheable = revision.area && length <= kCachedTagLength;
  size_t slot = 0;
  if (cacheable) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < length; ++i) hash = (hash ^ static_cast<unsigned char>(tag[i])) * 1099511628211ULL;
    slot = hash % kPrioritySlots;
    PriorityCache& cache = Priorities();
    AcquireSRWLockShared(&cache.lock);
    const PriorityEntry& entry = cache.entries[slot];
    bool hit = entry.revision == revision && entry.length == length &&
               (!length || std::memcmp(entry.tag.data(), tag, length) == 0);
    int priority = entry.priority;
    ReleaseSRWLockShared(&cache.lock);
    if (hit) return priority;
  }
  std::string names[4] = {"persist.log.tag", "log.tag"}, values[4];
  size_t count = 2;
  if (length) {
    std::string name(tag, length);
    names[2] = "persist.log.tag." + name;
    names[3] = "log.tag." + name;
    count = 4;
  }
  // CurrentRevision and GetAll may straddle a writer or device change. Cache
  // only the revision returned with the values, never the earlier observation.
  properties::GetAll(names, values, count, &revision);
  int property = count == 4 ? PropertyPriority(values[2], values[3]) : -1;
  if (property < 0) property = PropertyPriority(values[0], values[1]);
  if (cacheable && revision.area) {
    PriorityCache& cache = Priorities();
    AcquireSRWLockExclusive(&cache.lock);
    PriorityEntry& entry = cache.entries[slot];
    entry.revision = revision;
    entry.length = length;
    entry.priority = property;
    if (length) std::memcpy(entry.tag.data(), tag, length);
    ReleaseSRWLockExclusive(&cache.lock);
  }
  return property;
}
// Credential redaction (docs/logging.md). One pass over the message, without
// a regex engine: prefixed token formats are tested where their first letter
// occurs, credential-named keys only at '=' and ':' separators.
constexpr char kRedacted[] = "<redacted>";
constexpr size_t kRedactedSize = sizeof(kRedacted) - 1;
bool Alphanumeric(unsigned char c) {
  return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z');
}
// Base64/base64url/URL-safe token characters.
bool TokenChar(unsigned char c) {
  return Alphanumeric(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '+' || c == '/' || c == '=';
}
bool KeyChar(unsigned char c) { return Alphanumeric(c) || c == '_' || c == '-'; }
bool Blank(char c) { return c == ' ' || c == '\t'; }
// Where an unquoted key=value value ends (query strings, toString() fields, headers).
bool ValueEnd(char c) {
  return Blank(c) || c == '\r' || c == '\n' || c == ',' || c == ';' || c == '&' || c == ')' ||
         c == '}' || c == ']' || c == '"' || c == '\'';
}
// Whether |size| bytes at |text| equal the lower-case ASCII |word|, ignoring case.
bool SameWord(const char* text, size_t size, const char* word) {
  size_t length = std::strlen(word);
  if (size != length) return false;
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if ((c >= 'A' && c <= 'Z' ? c | 0x20 : c) != static_cast<unsigned char>(word[i])) return false;
  }
  return true;
}
bool EndsWithWord(const char* text, size_t size, const char* word) {
  size_t length = std::strlen(word);
  return size >= length && SameWord(text + size - length, length, word);
}
bool CredentialKey(const char* key, size_t size) {
  if (!size) return false;
  for (const char* suffix : {"token", "secret", "password", "passwd", "cookie"}) {
    if (EndsWithWord(key, size, suffix)) return true;
  }
  for (const char* name : {"authorization", "auth", "apikey", "api_key"}) {
    if (SameWord(key, size, name)) return true;
  }
  return false;
}
// Header-style values ("Bearer x", "a=1; b=2") run to the end of the line.
bool LineValue(const char* key, size_t size) {
  return SameWord(key, size, "authorization") || EndsWithWord(key, size, "cookie");
}
// The secret bytes [*begin, *end) a credential starting at |at| holds, if any.
bool Secret(const char* s, size_t n, size_t at, size_t* begin, size_t* end) {
  unsigned char c = static_cast<unsigned char>(s[at]);
  bool boundary = at == 0 || !Alphanumeric(static_cast<unsigned char>(s[at - 1]));
  // Google OAuth access tokens, Android master tokens, OAuth exchange codes and
  // refresh tokens. "1//" is common in other text, so it needs a token's length.
  static constexpr struct { const char* prefix; size_t length, minimum; bool boundary; } kPrefixes[] = {
      {"ya29.", 5, 1, false}, {"aas_et/", 7, 1, false}, {"oauth2_4/", 9, 1, false}, {"1//", 3, 20, true}};
  for (const auto& token : kPrefixes) {
    if (c != static_cast<unsigned char>(token.prefix[0]) || (token.boundary && !boundary) ||
        n - at < token.length || std::memcmp(s + at, token.prefix, token.length) != 0) {
      continue;
    }
    size_t j = at + token.length;
    while (j < n && TokenChar(static_cast<unsigned char>(s[j]))) ++j;
    if (j - (at + token.length) >= token.minimum) {
      *begin = at + token.length;
      *end = j;
      return true;
    }
  }
  if ((c | 0x20) == 'b' && boundary && n - at > 7 && SameWord(s + at, 6, "bearer") && Blank(s[at + 6])) {
    size_t v = at + 6;
    while (v < n && Blank(s[v])) ++v;
    size_t j = v;
    while (j < n && TokenChar(static_cast<unsigned char>(s[j]))) ++j;
    if (j > v) {
      *begin = v;
      *end = j;
      return true;
    }
  }
  if (c != '=' && c != ':') return false;
  // key=value, key: value, "key": "value", key='value', and the JSON forms
  // inside an escaped JSON string (\"key\":\"value\").
  size_t k = at;
  while (k > 0 && Blank(s[k - 1])) --k;
  if (k > 0 && (s[k - 1] == '"' || s[k - 1] == '\'')) {
    --k;
    if (k > 0 && s[k - 1] == '\\') --k;
  }
  size_t key_end = k;
  while (k > 0 && key_end - k < 64 && KeyChar(static_cast<unsigned char>(s[k - 1]))) --k;
  if (!CredentialKey(s + k, key_end - k)) return false;
  size_t v = at + 1;
  while (v < n && Blank(s[v])) ++v;
  size_t j = v;
  bool nested = v + 1 < n && s[v] == '\\' && (s[v + 1] == '"' || s[v + 1] == '\'');
  if (nested || (v < n && (s[v] == '"' || s[v] == '\''))) {
    // A quoted value ends at its closing quote, not at an escaped one. In an
    // escaped string each \x is one character of the quoted text.
    v += nested;
    char quote = s[v];
    j = ++v;
    bool escape = false;
    while (j < n) {
      size_t unit = nested && s[j] == '\\' && j + 1 < n ? 2 : 1;
      char c = s[j + unit - 1];
      if (c == '\n' || c == '\r' || (!escape && c == quote)) break;
      escape = !escape && c == '\\';
      j += unit;
    }
  } else if (LineValue(s + k, key_end - k)) {
    while (j < n && s[j] != '\n' && s[j] != '\r') ++j;
    while (j > v && Blank(s[j - 1])) --j;
  } else {
    while (j < n && !ValueEnd(s[j])) ++j;
  }
  // Already redacted (an earlier sink, or a scrubbed file): unchanged.
  if (j == v || (j - v == kRedactedSize && std::memcmp(s + v, kRedacted, kRedactedSize) == 0)) return false;
  *begin = v;
  *end = j;
  return true;
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
bool RedactCredentials(const char* text, size_t size, std::string* out) {
  if (!text) return false;
  size_t copied = 0;
  bool changed = false;
  for (size_t at = 0; at < size;) {
    size_t begin, end;
    if (!Secret(text, size, at, &begin, &end)) {
      ++at;
      continue;
    }
    changed = true;
    out->append(text + copied, begin - copied);
    out->append(kRedacted, kRedactedSize);
    copied = at = end;
  }
  if (changed) out->append(text + copied, size - copied);
  return changed;
}
bool IsLoggable(int priority, const char* tag, size_t length, int fallback, int minimum) {
  int property = TagPriority(tag, length);
  constexpr int kAndroidLogDefault = 1;  // android_LogPriority's default sentinel
  int threshold = property >= 0 ? property : minimum != kAndroidLogDefault ? minimum : fallback;
  if (property >= 0 && minimum != kAndroidLogDefault) threshold = std::min(property, minimum);
  return priority >= threshold;
}
void Record(int buffer, int priority, const char* tag, const char* text) {
  auto destination = CurrentDestination(nullptr);
  if (!destination) return;
  size_t tag_size = tag ? strnlen(tag, 4096) : 0, text_size = text ? strnlen(text, 4096) : 0;
  // Credentials never reach the file. Only a message holding one allocates;
  // the record's length fields describe the redacted text.
  std::string redacted;
  if (RedactCredentials(text, text_size, &redacted)) {
    text = redacted.data();
    text_size = std::min<size_t>(redacted.size(), 4096);
  }
  Header header{kMagic, static_cast<uint32_t>(sizeof(Header) + tag_size + text_size), 0,
                GetCurrentProcessId(), GetCurrentThreadId(), priority, buffer,
                static_cast<uint32_t>(tag_size), static_cast<uint32_t>(text_size)};
  // The existing wire format bounds each field to 4096 bytes. Keep the one
  // atomic append and rollback path without a heap allocation per record.
  std::array<char, sizeof(Header) + 2 * 4096> data;
  if (tag_size) std::memcpy(data.data() + sizeof(header), tag, tag_size);
  if (text_size) std::memcpy(data.data() + sizeof(header) + tag_size, text, text_size);
  Lock lock(destination->mutex.value);
  if (!lock.held) return;
  // Declared after Lock: close the data handle before releasing the mutex.
  Handle file;
  if (!OpenFile(*destination, &file, nullptr)) return;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 0) return;
  if (size.QuadPart > kLimit - static_cast<int64_t>(header.size)) {
    if (!Truncate(file.value)) return;
    size.QuadPart = 0;
  }
  // Stamped under the lock, so records reflect their serialization order.
  FILETIME time;
  GetSystemTimePreciseAsFileTime(&time);
  header.timestamp = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  std::memcpy(data.data(), &header, sizeof(header));
  OVERLAPPED at{};
  at.Offset = static_cast<DWORD>(size.QuadPart);
  at.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(size.QuadPart) >> 32);
  DWORD written = 0;
  if (!WriteFile(file.value, data.data(), header.size, &written, &at) ||
      written != header.size) {
    // Best effort: roll back a partial append so later reads keep valid records.
    Resize(file.value, size.QuadPart);
  }
}
bool Read(std::vector<Entry>* entries, std::string* error) {
  auto destination = CurrentDestination(error);
  if (!destination) return false;
  Lock lock(destination->mutex.value);
  if (!lock.held) { if (error) *error = "cannot lock the log"; return false; }
  Handle file;
  if (!OpenFile(*destination, &file, error)) return false;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.value, &size) || size.QuadPart > kLimit || size.QuadPart < 0) {
    if (error) *error = "invalid log size";
    return false;
  }
  std::string bytes(static_cast<size_t>(size.QuadPart), 0);
  DWORD count = 0;
  if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size()) {
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
  auto destination = CurrentDestination(error);
  if (!destination) return false;
  Lock lock(destination->mutex.value);
  if (!lock.held) { if (error) *error = "cannot lock the log"; return false; }
  Handle file;
  if (!OpenFile(*destination, &file, error)) return false;
  if (!Truncate(file.value)) { if (error) *error = "cannot clear the log"; return false; }
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
