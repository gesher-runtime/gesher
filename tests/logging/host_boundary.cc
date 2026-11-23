// Host-only boundaries, never linked into Gesher. Log cache/assembly, formats,
// append/rollback and locking are the unedited production logging.cc.
#include <windows.h>
#include <gesher/system_properties.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <strings.h>
#include <thread>
#include <vector>
#include <unistd.h>
extern constinit thread_local bool count_allocations;
extern constinit thread_local uint64_t kernel_waits, file_opens, file_sizes, file_writes, file_closes;
extern std::atomic<int> short_write;
extern std::atomic<bool> fail_write;
namespace {
struct Uncounted { bool saved = count_allocations; Uncounted() { count_allocations = false; } ~Uncounted() { count_allocations = saved; } };
std::recursive_mutex log_mutex;
std::mutex environment_mutex;
std::map<std::wstring, std::wstring> environment;
std::map<std::wstring, std::vector<char>> files;
struct File { std::vector<char>* bytes; size_t offset = 0; };
HANDLE mutex_handle = reinterpret_cast<HANDLE>(1);
}
DWORD GetEnvironmentVariableW(LPCWSTR name, wchar_t* output, DWORD size) {
  Uncounted pause;
  std::lock_guard lock(environment_mutex);
  auto found = environment.find(name);
  if (found == environment.end() || found->second.empty()) return 0;
  const auto& value = found->second;
  if (size <= value.size()) return static_cast<DWORD>(value.size() + 1);
  std::wmemcpy(output, value.c_str(), value.size() + 1);
  return static_cast<DWORD>(value.size());
}
BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value) {
  Uncounted pause;
  std::lock_guard lock(environment_mutex);
  if (value) environment[name] = value; else environment.erase(name);
  return TRUE;
}
DWORD GetFullPathNameW(LPCWSTR path, DWORD size, wchar_t* output, wchar_t**) {
  Uncounted pause;
  std::wstring value(path);
  if (value.size() < 3 || value[1] != L':') value = L"C:\\host\\" + value;
  if (size <= value.size()) return static_cast<DWORD>(value.size() + 1);
  std::wmemcpy(output, value.c_str(), value.size() + 1);
  return static_cast<DWORD>(value.size());
}
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR) { return mutex_handle; }
DWORD WaitForSingleObject(HANDLE value, DWORD) { if (value != mutex_handle) std::abort(); ++kernel_waits; log_mutex.lock(); return WAIT_OBJECT_0; }
BOOL ReleaseMutex(HANDLE) { log_mutex.unlock(); return TRUE; }
BOOL CloseHandle(HANDLE value) { Uncounted pause; if (value != mutex_handle) { ++file_closes; delete static_cast<File*>(value); } return TRUE; }
DWORD GetLastError() { return 2; }
HANDLE CreateFileW(LPCWSTR path, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) {
  Uncounted pause;
  ++file_opens;
  return new File{&files[path]};
}
BOOL GetFileSizeEx(HANDLE handle, LARGE_INTEGER* size) { ++file_sizes; size->QuadPart = static_cast<File*>(handle)->bytes->size(); return TRUE; }
BOOL SetFilePointerEx(HANDLE handle, LARGE_INTEGER value, LARGE_INTEGER*, DWORD) { static_cast<File*>(handle)->offset = value.QuadPart; return TRUE; }
BOOL SetEndOfFile(HANDLE handle) { Uncounted pause; auto* file = static_cast<File*>(handle); file->bytes->resize(file->offset); return TRUE; }
BOOL WriteFile(HANDLE handle, LPCVOID data, DWORD size, LPDWORD written, OVERLAPPED* at) {
  Uncounted pause;
  ++file_writes;
  auto* file = static_cast<File*>(handle);
  size_t offset = at ? (static_cast<uint64_t>(at->OffsetHigh) << 32) | at->Offset : file->offset;
  int limit = short_write.exchange(-1);
  *written = limit < 0 ? size : std::min(size, static_cast<DWORD>(limit));
  file->bytes->resize(std::max(file->bytes->size(), offset + *written));
  std::memcpy(file->bytes->data() + offset, data, *written);
  return fail_write.exchange(false) ? FALSE : TRUE;
}
BOOL ReadFile(HANDLE handle, LPVOID data, DWORD size, LPDWORD read, OVERLAPPED*) {
  auto* file = static_cast<File*>(handle);
  *read = std::min<size_t>(size, file->bytes->size());
  if (*read) std::memcpy(data, file->bytes->data(), *read);
  return TRUE;
}
BOOL DeleteFileW(LPCWSTR path) { Uncounted pause; return files.erase(path) != 0; }
BOOL MoveFileW(LPCWSTR from, LPCWSTR to) { Uncounted pause; auto found = files.find(from); if (found == files.end()) return FALSE; files[to] = std::move(found->second); files.erase(found); return TRUE; }
DWORD GetCurrentProcessId() { return getpid(); }
DWORD GetCurrentThreadId() { return static_cast<DWORD>(std::hash<std::thread::id>{}(std::this_thread::get_id())); }
void GetSystemTimePreciseAsFileTime(FILETIME* time) { static std::atomic<uint64_t> ticks{116444736000000000ULL}; uint64_t now = ++ticks; *time = {static_cast<DWORD>(now), static_cast<DWORD>(now >> 32)}; }
BOOL FileTimeToLocalFileTime(const FILETIME* utc, FILETIME* local) { *local = *utc; return TRUE; }
BOOL FileTimeToSystemTime(const FILETIME*, SYSTEMTIME* time) { *time = {2026, 11, 0, 22, 1, 2, 3, 4}; return TRUE; }
int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
namespace gesher::properties {
namespace {
std::mutex mutex;
std::map<std::string, std::string> values;
uint32_t generation = 1;
}
bool Initialize() { return true; }
bool Set(const std::string& name, const std::string& value) {
  if (name.empty() || name.size() > 255 || name.back() == '.' || name.find('\0') != std::string::npos) return false;
  std::lock_guard lock(mutex); values[name] = value; ++generation; return true;
}
#if LOGGING_HAS_REVISION
Revision CurrentRevision() { std::lock_guard lock(mutex); return {&values, generation}; }
void GetAll(const std::string* names, std::string* output, size_t count, Revision* revision) {
#else
void GetAll(const std::string* names, std::string* output, size_t count) {
#endif
  std::lock_guard lock(mutex);
#if LOGGING_HAS_REVISION
  if (revision) *revision = {&values, generation};
#endif
  for (size_t i = 0; i < count; ++i) { auto found = values.find(names[i]); output[i] = found == values.end() ? std::string{} : found->second; }
}
}
