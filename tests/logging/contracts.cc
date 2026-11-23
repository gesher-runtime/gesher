// NATIVE-13: production logger and (on Windows) real property area contracts.
// Linux adapts only the Win32/storage boundary; its timing is not Windows I/O.
#include <gesher/logging.h>
#include <gesher/system_properties.h>
#if LOGGING_HAS_ENVIRONMENT_OWNER
#include <gesher/environment.h>
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <psapi.h>
#else
#include <time.h>
#endif

constinit thread_local bool count_allocations = false;
constinit thread_local uint64_t allocations = 0, allocated_bytes = 0;
constinit thread_local uint64_t kernel_waits = 0, file_opens = 0, file_sizes = 0, file_writes = 0, file_closes = 0;
std::atomic<int> short_write{-1};
std::atomic<bool> fail_write{false};
void* operator new(size_t size) {
  if (count_allocations) { ++allocations; allocated_bytes += size; }
  void* result = std::malloc(size ? size : 1);
  if (!result) std::abort();
  return result;
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }
#ifdef _WIN32
DWORD WINAPI LoggingTestWait(HANDLE value, DWORD timeout) { ++kernel_waits; return WaitForSingleObject(value, timeout); }
HANDLE WINAPI LoggingTestOpen(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                             DWORD creation, DWORD flags, HANDLE template_file) {
  ++file_opens; return CreateFileW(path, access, share, security, creation, flags, template_file);
}
BOOL WINAPI LoggingTestSize(HANDLE value, LARGE_INTEGER* size) { ++file_sizes; return GetFileSizeEx(value, size); }
BOOL WINAPI LoggingTestWrite(HANDLE file, LPCVOID data, DWORD size, LPDWORD written, LPOVERLAPPED at) {
  ++file_writes;
  int limit = short_write.exchange(-1);
  BOOL result = WriteFile(file, data, limit < 0 ? size : std::min(size, static_cast<DWORD>(limit)), written, at);
  return fail_write.exchange(false) ? FALSE : result;
}
BOOL WINAPI LoggingTestClose(HANDLE value) { ++file_closes; return CloseHandle(value); }
#endif

namespace {
unsigned checks = 0, failures = 0;
bool measure_only = false;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) { ++failures; std::printf("FAIL structured logging: %s\n", message); }
}
void ResetCounters() { allocations = allocated_bytes = kernel_waits = file_opens = file_sizes = file_writes = file_closes = 0; }
uint64_t CpuNs() {
#ifdef _WIN32
  FILETIME created, ended, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &ended, &kernel, &user)) std::abort();
  return (((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
          ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime)) * 100;
#else
  timespec value{}; if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value)) std::abort();
  return uint64_t(value.tv_sec) * 1000000000ULL + value.tv_nsec;
#endif
}
int Parse(const std::string& value) {
  if (value.empty()) return -1;
  if (value == "false" || value == "FaLsE") return 8;
  for (const auto& entry : {std::pair{'V', 2}, {'D', 3}, {'I', 4}, {'W', 5}, {'E', 6}, {'F', 7}, {'A', 7}, {'S', 8}}) {
    if (value[0] == entry.first || value[0] == entry.first + ('a' - 'A')) return entry.second;
  }
  return -1;
}
// Separate priority oracle: resolve nonempty pairs, then choose the available
// defaults. No production helpers or production cache layout are reused.
int Expected(const std::string& persistent, const std::string& transient,
             const std::string& global_persistent, const std::string& global_transient,
             int fallback, int minimum) {
  int level = Parse(!persistent.empty() ? persistent : transient);
  if (level < 0) level = Parse(!global_persistent.empty() ? global_persistent : global_transient);
  if (level < 0) return minimum == 1 ? fallback : minimum;
  return minimum == 1 || minimum > level ? level : minimum;
}
void Priorities() {
  const std::string tag = "native13.priority";
  struct Row { const char* persistent; const char* transient; const char* global_persistent; const char* global_transient; };
  const Row cases[] = {{"", "", "", ""}, {"", "D", "", "E"}, {"W", "D", "", "V"},
      {"", "", "I", "E"}, {"invalid", "V", "", "W"}, {"true", "E", "", "D"},
      {"false", "V", "", "V"}, {"FaLsE", "", "", "E"}, {"", "verbose", "", "E"},
      {"", "A", "", "V"}, {"", "", "invalid", "V"}, {"", "", "", "s"}};
  for (const Row& row : cases) {
    Check(gesher::properties::Set("persist.log.tag." + tag, row.persistent), "set persistent tag priority");
    Check(gesher::properties::Set("log.tag." + tag, row.transient), "set transient tag priority");
    Check(gesher::properties::Set("persist.log.tag", row.global_persistent), "set persistent global priority");
    Check(gesher::properties::Set("log.tag", row.global_transient), "set transient global priority");
    for (int minimum : {1, 2, 4, 8}) for (int fallback : {2, 4, 6}) for (int priority = 0; priority < 10; ++priority) {
      int threshold = Expected(row.persistent, row.transient, row.global_persistent, row.global_transient, fallback, minimum);
      Check(gesher::logging::IsLoggable(priority, tag.data(), tag.size(), fallback, minimum) == (priority >= threshold),
            "tag precedence, dynamic minimum and fallback match oracle after mutation");
      threshold = Expected("", "", row.global_persistent, row.global_transient, fallback, minimum);
      Check(gesher::logging::IsLoggable(priority, nullptr, 999, fallback, minimum) == (priority >= threshold),
            "null tag ignores explicit length and matches global oracle");
    }
  }
  Check(gesher::properties::Set("persist.log.tag", ""), "clear persistent global");
  Check(gesher::properties::Set("log.tag", "E"), "global threshold for length tests");
  Check(gesher::properties::Set("log.tag.abc", "D"), "set prefix tag");
  Check(gesher::logging::IsLoggable(3, "abc-not-terminated-at-length", 3, 4, 1), "explicit tag length does not read suffix");
  Check(!gesher::logging::IsLoggable(3, "abc\0suffix", 10, 4, 1), "embedded NUL does not alias shorter cached tag");
  for (size_t length : {239U, 247U, 248U, 4096U}) {
    std::string long_tag(length, 'x');
    bool set = gesher::properties::Set("log.tag." + long_tag, "D");
    Check(set == (length <= 247), "property name bounds retained for long tags");
    Check(gesher::logging::IsLoggable(3, long_tag.data(), long_tag.size(), 4, 1) == set, "long explicit tags retain fallback and property semantics");
  }
  // More distinct tags than cache slots force eviction/collisions; an exact
  // tag match must still select each independently configured priority.
  const char* levels[] = {"V", "D", "I", "W", "E", "F", "S"};
  std::vector<std::string> tags;
  for (unsigned i = 0; i < 160; ++i) {
    tags.push_back("native13.churn." + std::to_string(i));
    Check(gesher::properties::Set("log.tag." + tags.back(), levels[i % 7]), "configure many-tag cache churn");
  }
  for (unsigned pass = 0; pass < 3; ++pass) for (unsigned i = 0; i < tags.size(); ++i) {
    unsigned at = pass % 2 ? unsigned(tags.size()) - 1 - i : i;
    int threshold = 2 + at % 7;
    Check(!gesher::logging::IsLoggable(threshold - 1, tags[at].data(), tags[at].size(), 4, 1) &&
          gesher::logging::IsLoggable(threshold, tags[at].data(), tags[at].size(), 4, 1),
          "many-tag eviction never aliases a different priority");
  }
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (int i = 0; i < 150; ++i) if (!gesher::properties::Set("log.tag.concurrent", i % 2 ? "E" : "D")) std::abort();
    done.store(true);
  });
  while (!done.load()) {
    Check(!gesher::logging::IsLoggable(2, "concurrent", 10, 4, 1), "concurrent priorities never fabricate VERBOSE");
    Check(gesher::logging::IsLoggable(6, "concurrent", 10, 4, 1), "concurrent priorities always permit ERROR");
  }
  writer.join();
  Check(!gesher::logging::IsLoggable(3, "concurrent", 10, 4, 1), "completed writer invalidates warmed tag cache");
}
bool Texts(std::initializer_list<const char*> expected) {
  std::vector<gesher::logging::Entry> values; std::string error;
  if (!gesher::logging::Read(&values, &error) || values.size() != expected.size()) return false;
  size_t i = 0; for (const char* text : expected) if (values[i++].text != text) return false;
  return true;
}
void Records(const std::wstring& path) {
  std::string error;
  Check(gesher::logging::Clear(&error), "clear initial log");
  gesher::logging::Record(3, 7, "native13", "before");
  for (bool failed : {false, true}) {
    short_write = 43; fail_write = failed;
    gesher::logging::Record(2, 4, "partial", "must roll back");
    Check(Texts({"before"}), "short and failed writes roll back exact previous file size");
  }
  gesher::logging::Record(2, 4, "native13", "after");
  Check(Texts({"before", "after"}), "next append follows rollback with no corrupt gap");
  Check(DeleteFileW(path.c_str()), "idle logger keeps no file handle preventing delete");
  gesher::logging::Record(0, 4, "native13", "recreated");
  Check(Texts({"recreated"}), "append recreates deleted destination");
  std::wstring archive = path + L".old";
  Check(MoveFileW(path.c_str(), archive.c_str()), "rotate idle destination");
  gesher::logging::Record(0, 7, "native13", "fatal");
  Check(Texts({"fatal"}), "fatal record immediately readable in replacement file");
  Check(gesher::logging::Clear(&error) && Texts({}), "clear is synchronous");
  std::string large_tag(4200, 't'), large_text(4200, 'm');
  gesher::logging::Record(-3, 9, large_tag.c_str(), large_text.c_str());
  gesher::logging::Record(0, 4, nullptr, nullptr);
  std::vector<gesher::logging::Entry> values;
  Check(gesher::logging::Read(&values, &error) && values.size() == 2, "complete binary records parse");
  if (values.size() == 2) {
    Check(values[0].tag == large_tag.substr(0, 4096) && values[0].text == large_text.substr(0, 4096) &&
          values[0].buffer == -3 && values[0].priority == 9 && values[0].pid == GetCurrentProcessId() &&
          values[0].tid == GetCurrentThreadId() && values[0].timestamp != 0,
          "binary field lengths, contents, IDs, priority and buffer preserved");
    Check(values[1].tag.empty() && values[1].text.empty(), "null tag/text preserve empty fields");
  }
  Check(gesher::logging::Clear(&error), "clear before concurrent records");
  std::vector<std::thread> writers;
  for (int thread = 0; thread < 4; ++thread) writers.emplace_back([thread] {
    std::string tag = std::to_string(thread);
    for (int i = 0; i < 200; ++i) { std::string text = std::to_string(i); gesher::logging::Record(0, 4, tag.c_str(), text.c_str()); }
  });
  for (auto& writer : writers) writer.join();
  Check(gesher::logging::Read(&values, &error) && values.size() == 800, "concurrent writers retain every complete record");
  int next[4]{};
  bool ordered = true;
  for (const auto& value : values) {
    int id = std::atoi(value.tag.c_str());
    if (id < 0 || id >= 4 || value.text != std::to_string(next[id]++)) ordered = false;
  }
  Check(ordered, "concurrent appends preserve each writer's order");
  Check(DeleteFileW(archive.c_str()), "archive cleanup");
}
#ifdef _WIN32
#if LOGGING_HAS_ENVIRONMENT_OWNER
bool OwnerMatchesNative(const char* name, const wchar_t* wide_name) {
  const char* owned = gesher::GetEnvironment(name);
  if (!owned) return false;
  wchar_t native[32768];
  DWORD length = GetEnvironmentVariableW(wide_name, native, 32768);
  if (!length || length >= 32768) return false;
  std::wstring decoded(length + 1, L'\0');
  int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, owned, -1,
                                    decoded.data(), static_cast<int>(decoded.size()));
  return converted == static_cast<int>(length + 1) &&
         std::wmemcmp(decoded.data(), native, length + 1) == 0;
}
void PropertyEnvironmentOwner() {
  Check(OwnerMatchesNative("GESHER_PROPERTY_ROOT", L"GESHER_PROPERTY_ROOT"),
        "shared POSIX owner exposes the actual inherited property root");
  Check(OwnerMatchesNative("GESHER_PROPERTY_ROOT_SESSION", L"GESHER_PROPERTY_ROOT_SESSION"),
        "shared POSIX owner exposes the actual inherited property session");
}
#endif
bool Child(const wchar_t* argument, DWORD expected = 0) {
  wchar_t executable[32768];
  DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
  if (!length || length >= 32768) return false;
  std::wstring command = L"\"" + std::wstring(executable, length) + L"\" " + argument;
  STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return false;
  bool complete = WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0;
  DWORD status = 1;
  if (complete) GetExitCodeProcess(process.hProcess, &status);
  else { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000); }
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
  return complete && status == expected;
}
void WindowsIdentity(const std::wstring& session) {
  Check(gesher::properties::Set("log.tag.child", "D"), "warm cross-process priority");
  Check(gesher::logging::IsLoggable(3, "child", 5, 4, 1), "initial cross-process priority");
  Check(Child(L"--publish-priority"), "child publishes real mapped property generation");
  Check(!gesher::logging::IsLoggable(3, "child", 5, 4, 1), "cross-process generation invalidates log cache");
  for (const auto& suffix : {L"-A", L"-B", L"-A", L"-B"}) {
    std::wstring next = session + suffix;
    Check(SetEnvironmentVariableW(L"GESHER_BINDER", next.c_str()), "switch real property area");
    std::string value;
    if (!gesher::properties::Get("log.tag.identity", &value))
      Check(gesher::properties::Set("log.tag.identity", suffix[1] == 'A' ? "D" : "E"), "initialize equal-generation area");
    Check(gesher::logging::IsLoggable(3, "identity", 8, 4, 1) == (suffix[1] == 'A'),
          "equal generations from different areas never share tag cache");
  }
  Check(SetEnvironmentVariableW(L"GESHER_BINDER", session.c_str()), "restore original property area");
  Check(!gesher::logging::IsLoggable(3, "child", 5, 4, 1), "warm restored original area before damaging its inherited root");
  Check(SetEnvironmentVariableW(L"GESHER_PROPERTY_ROOT", L"incorrect-inherited-root"), "damage inherited root binding");
  Check(gesher::logging::IsLoggable(6, "child", 5, 4, 1), "cache lookup repairs inherited root binding");
#if LOGGING_HAS_ENVIRONMENT_OWNER
  PropertyEnvironmentOwner();
#endif
  Check(Child(L"--publish-repaired-root"), "child publishes a distinct priority after inherited root repair");
  std::string repaired;
  Check(gesher::properties::Get("log.tag.child", &repaired) && repaired == "V",
        "parent original mapping observes the child's distinct publication through the repaired root");
  Check(gesher::logging::IsLoggable(2, "child", 5, 4, 1), "repaired-root child publication invalidates the original logging cache");
  std::string error;
  Check(gesher::logging::Clear(&error), "clear before immediate-exit child");
  Check(Child(L"--fatal-exit", 23), "recording child exits without destructor or asynchronous flush");
  Check(Texts({"fatal-exit"}), "fatal record visible after immediate process exit");
}
#endif
void Measure() {
  const std::string tag = "native13.performance.long.enough.to.allocate";
  Check(gesher::properties::Set("log.tag." + tag, "E"), "set benchmark priority");
  const std::string text(192, 'x');
  gesher::logging::IsLoggable(3, tag.data(), tag.size(), 4, 1);
  gesher::logging::Record(0, 6, tag.c_str(), text.c_str());
  for (bool enabled : {false, true}) for (unsigned repeat = 0; repeat < 5; ++repeat) {
    unsigned count = enabled ? 2000 : 200000;
    ResetCounters();
    uint64_t cpu = CpuNs(); auto start = std::chrono::steady_clock::now();
    count_allocations = true;
    unsigned observed = 0;
    for (unsigned i = 0; i < count; ++i) if (gesher::logging::IsLoggable(enabled ? 6 : 3, tag.data(), tag.size(), 4, 1)) {
      ++observed; gesher::logging::Record(0, 6, tag.c_str(), text.c_str());
    }
    count_allocations = false;
    cpu = CpuNs() - cpu;
    auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    std::printf("MEASURE structured_logging enabled=%d repeat=%u operations=%u cpu_ns=%llu wall_ns=%lld allocations=%llu allocated_bytes=%llu mutex_waits=%llu opens=%llu sizes=%llu writes=%llu closes=%llu\n",
      enabled, repeat, count, static_cast<unsigned long long>(cpu), static_cast<long long>(wall),
      static_cast<unsigned long long>(allocations), static_cast<unsigned long long>(allocated_bytes),
      static_cast<unsigned long long>(kernel_waits), static_cast<unsigned long long>(file_opens),
      static_cast<unsigned long long>(file_sizes), static_cast<unsigned long long>(file_writes), static_cast<unsigned long long>(file_closes));
    Check(observed == (enabled ? count : 0), "benchmark preserves enabled/disabled result");
    if (!measure_only) {
      Check(allocations == 0, "warm ordinary tag and destination allocate no C++ heap memory");
      Check(kernel_waits == (enabled ? count : 0), "cache hits add no cross-process property mutex waits");
    }
    Check(file_opens == (enabled ? count : 0) && file_sizes == file_opens && file_writes == file_opens && file_closes == file_opens,
          "one synchronous open/size/write/close per enabled record; no disabled I/O");
  }
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX memory{};
  Check(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)), "read real process memory");
  std::printf("RESOURCE structured_logging private_commit_bytes=%llu working_set_bytes=%llu peak_working_set_bytes=%llu\n",
              static_cast<unsigned long long>(memory.PrivateUsage), static_cast<unsigned long long>(memory.WorkingSetSize),
              static_cast<unsigned long long>(memory.PeakWorkingSetSize));
#else
  std::puts("RESOURCE structured_logging Windows private commit / working set / kernel I/O latency unavailable on host boundary");
#endif
}
}
int main(int argc, char** argv) {
#ifdef _WIN32
  if (argc == 2 && std::strcmp(argv[1], "--publish-priority") == 0) return gesher::properties::Set("log.tag.child", "E") ? 0 : 1;
  if (argc == 2 && std::strcmp(argv[1], "--publish-repaired-root") == 0) return gesher::properties::Set("log.tag.child", "V") ? 0 : 1;
  if (argc == 2 && std::strcmp(argv[1], "--fatal-exit") == 0) { gesher::logging::Record(0, 7, "fatal", "fatal-exit"); ExitProcess(23); }
  wchar_t temporary[32768]; DWORD length = GetTempPathW(32768, temporary);
  if (!length || length >= 32768) return 1;
  std::wstring session = L"native13-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
  std::wstring directory = std::wstring(temporary, length) + session;
  std::filesystem::create_directories(directory);
  SetEnvironmentVariableW(L"ANDROID_DATA", directory.c_str());
  SetEnvironmentVariableW(L"ANDROID_ROOT", nullptr);
  SetEnvironmentVariableW(L"GESHER_BINDER", session.c_str());
  SetEnvironmentVariableW(L"GESHER_PROPERTY_ROOT", nullptr);
  SetEnvironmentVariableW(L"GESHER_PROPERTY_ROOT_SESSION", nullptr);
  std::wstring path = directory + L"\\current.log";
  std::puts("BOUNDARY actual Windows files, named mutexes, mapped property area, child processes; exact production sources");
#else
  std::wstring path = L"C:\\host\\native13-logging.log";
  std::puts("BOUNDARY complete production logger; Win32 and property storage adapters; host CPU is not Windows I/O timing");
#endif
  measure_only = argc == 2 && std::strcmp(argv[1], "--measure-only") == 0;
  Check(SetEnvironmentVariableW(L"GESHER_LOG_FILE", path.c_str()), "configure destination");
  Check(gesher::properties::Initialize(), "initialize property area");
#if LOGGING_HAS_ENVIRONMENT_OWNER
  PropertyEnvironmentOwner();
#endif
  Priorities(); Records(path);
#ifdef _WIN32
  WindowsIdentity(session);
#endif
  Measure();
  Check(DeleteFileW(path.c_str()), "delete final log without held data handles");
  std::printf("structured logging: %u checks, %u failed\n", checks, failures);
  return failures ? 1 : 0;
}
