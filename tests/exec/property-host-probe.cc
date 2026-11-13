// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Native regressions for the real host libutils callback path and boot area.
#include <gesher/logging.h>
#include <gesher/system_properties.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
namespace android {
void add_sysprop_change_callback(void (*)(), int);
void report_sysprop_change();
}
namespace {
std::vector<int> observed;
void Later() { observed.push_back(40); }
void First() {
  observed.push_back(30);
  android::add_sysprop_change_callback(Later, 40);  // must not hold the dispatch lock
}
void Second() { observed.push_back(20); }
void Third() { observed.push_back(10); }
struct Handle {
  HANDLE value = nullptr;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Child {
  PROCESS_INFORMATION process{};
  ~Child() {
    if (process.hProcess) {
      if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
      }
      CloseHandle(process.hProcess);
    }
    if (process.hThread) CloseHandle(process.hThread);
  }
  bool Start(const std::wstring& arguments) {
    std::vector<wchar_t> path(32768);
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return false;
    std::wstring command = L"\"" + std::wstring(path.data(), length) + L"\" " + arguments;
    std::vector<wchar_t> writable(command.begin(), command.end());
    writable.push_back(0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    return CreateProcessW(path.data(), writable.data(), nullptr, nullptr, FALSE, 0,
                           nullptr, nullptr, &startup, &process) != 0;
  }
  bool Finish() {
    DWORD status = 1;
    return process.hProcess && WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0 &&
           GetExitCodeProcess(process.hProcess, &status) && status == 0;
  }
};
std::wstring WideAscii(const std::string& value) { return {value.begin(), value.end()}; }
std::wstring Environment(const wchar_t* name) {
  DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
  if (!size) return {};
  std::wstring result(size, L'\0');
  DWORD count = GetEnvironmentVariableW(name, result.data(), size);
  if (!count || count >= size) return {};
  result.resize(count);
  return result;
}
struct LogEnvironment {
  std::wstring saved = Environment(L"GESHER_LOG_FILE");
  ~LogEnvironment() { SetEnvironmentVariableW(L"GESHER_LOG_FILE", saved.empty() ? nullptr : saved.c_str()); }
};
bool LogIs(std::initializer_list<const char*> expected) {
  std::vector<gesher::logging::Entry> entries;
  std::string error;
  if (!gesher::logging::Read(&entries, &error) || entries.size() != expected.size()) return false;
  size_t at = 0;
  for (const char* text : expected) if (entries[at++].text != text) return false;
  return true;
}
int IdleLogger(const std::string& prefix) {
  Handle ready, resume;
  ready.value = OpenEventW(EVENT_MODIFY_STATE, FALSE, WideAscii(prefix + "-ready").c_str());
  resume.value = OpenEventW(SYNCHRONIZE, FALSE, WideAscii(prefix + "-resume").c_str());
  if (!ready.value || !resume.value) return 1;
  gesher::logging::Record(0, 4, "lifetime", "idle-before");
  if (!SetEvent(ready.value) || WaitForSingleObject(resume.value, 30000) != WAIT_OBJECT_0) return 1;
  gesher::logging::Record(0, 4, "lifetime", "idle-resumed");
  return 0;
}
bool LogRegressions() {
  LogEnvironment environment;
  bool ok = true;
  auto verify = [&](const char* label, bool result) {
    if (!result) { std::printf("FAIL structured log: %s\n", label); ok = false; }
    return result;
  };
  wchar_t temp[32768];
  DWORD length = GetTempPathW(32768, temp);
  if (!verify("temporary path", length && length < 32768)) return false;
  std::wstring directory = std::wstring(temp, length) + L"gesher-log-" +
      std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
  if (!verify("temporary directory", CreateDirectoryW(directory.c_str(), nullptr))) return false;
  std::wstring path = directory + L"\\current.log", archive = directory + L"\\archive.log";
  if (!verify("configure log", SetEnvironmentVariableW(L"GESHER_LOG_FILE", path.c_str()))) return false;
  gesher::logging::Record(0, 4, "lifetime", "parent-before");
  std::string prefix = "Local\\gesher-log-test-" + std::to_string(GetCurrentProcessId()) +
                       "-" + std::to_string(GetTickCount64());
  Handle ready, resume;
  ready.value = CreateEventW(nullptr, TRUE, FALSE, WideAscii(prefix + "-ready").c_str());
  resume.value = CreateEventW(nullptr, TRUE, FALSE, WideAscii(prefix + "-resume").c_str());
  Child child;
  if (!verify("create child barriers", ready.value && resume.value) ||
      !verify("start idle writer", child.Start(L"--idle-logger " + WideAscii(prefix))) ||
      !verify("idle writer has completed its first record",
              WaitForSingleObject(ready.value, 30000) == WAIT_OBJECT_0)) return false;
  verify("both writers reached the log", LogIs({"parent-before", "idle-before"}));
  DWORD status = 0;
  verify("other writer remains alive and idle", GetExitCodeProcess(child.process.hProcess, &status) && status == STILL_ACTIVE);
  verify("delete while both writers are idle", DeleteFileW(path.c_str()));
  gesher::logging::Record(0, 4, "lifetime", "after-delete");
  verify("recreate before the idle writer resumes", LogIs({"after-delete"}));
  verify("resume idle writer", SetEvent(resume.value));
  verify("idle writer exits successfully", child.Finish());
  verify("idle writer follows the recreated path", LogIs({"after-delete", "idle-resumed"}));
  verify("rotate open destination", MoveFileW(path.c_str(), archive.c_str()));
  gesher::logging::Record(0, 4, "lifetime", "replacement");
  verify("new records use the replacement", LogIs({"replacement"}));
  verify("select archive", SetEnvironmentVariableW(L"GESHER_LOG_FILE", archive.c_str()));
  verify("rotation left archive unchanged", LogIs({"after-delete", "idle-resumed"}));
  verify("restore current path", SetEnvironmentVariableW(L"GESHER_LOG_FILE", path.c_str()));
  Child clearer;
  verify("clear from another process", clearer.Start(L"--clear-log") && clearer.Finish());
  gesher::logging::Record(0, 4, "lifetime", "after-clear");
  verify("append follows cross-process clear", LogIs({"after-clear"}));

  // Fill beyond the 16 MiB boundary; the remaining complete records are the
  // suffix after one rollover, never sparse bytes from a stale file offset.
  std::string error, payload(4096, 'x');
  verify("clear before rollover", gesher::logging::Clear(&error));
  constexpr size_t record_size = 40 + 4 + 4096;
  constexpr size_t capacity = 16 * 1024 * 1024 / record_size;
  for (size_t i = 0; i < capacity + 3; ++i) gesher::logging::Record(0, 4, "roll", payload.c_str());
  std::vector<gesher::logging::Entry> entries;
  verify("rollover preserves complete suffix", gesher::logging::Read(&entries, &error) &&
         entries.size() == 3 && std::all_of(entries.begin(), entries.end(),
         [&](const auto& entry) { return entry.tag == "roll" && entry.text == payload; }));

  // Extended Win32 paths were already accepted by the logger. Exercise a
  // name longer than the removed 2048-character FileNameInfo buffer.
  std::wstring long_root = directory.rfind(L"\\\\?\\", 0) == 0 ? directory :
      directory.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + directory.substr(2) : L"\\\\?\\" + directory;
  std::vector<std::wstring> directories;
  std::wstring deep = long_root;
  while (deep.size() < 2200) {
    deep += L"\\" + std::wstring(100, L'd');
    if (!verify("create long-path component", CreateDirectoryW(deep.c_str(), nullptr))) break;
    directories.push_back(deep);
  }
  if (deep.size() >= 2200) {
    std::wstring long_log = deep + L"\\long.log";
    verify("select long log path", SetEnvironmentVariableW(L"GESHER_LOG_FILE", long_log.c_str()));
    gesher::logging::Record(0, 4, "long", "long-path");
    verify("read long log path", LogIs({"long-path"}));
    verify("delete long log", DeleteFileW(long_log.c_str()));
  }
  verify("restore log path for cleanup", SetEnvironmentVariableW(L"GESHER_LOG_FILE", path.c_str()));
  for (auto it = directories.rbegin(); it != directories.rend(); ++it) verify("remove long-path component", RemoveDirectoryW(it->c_str()));
  verify("delete current", DeleteFileW(path.c_str()));
  verify("delete archive", DeleteFileW(archive.c_str()));
  verify("remove temporary directory", RemoveDirectoryW(directory.c_str()));
  return ok;
}
bool RefreshAcrossProcesses(const std::string& name) {
  std::string value;
  uint32_t old_serial = 0;
  if (!gesher::properties::Set(name, "old") ||
      !gesher::properties::Get(name, &value, &old_serial) || value != "old") return false;
  Child child;
  if (!child.Start(L"--publish " + WideAscii(name)) || !child.Finish()) return false;
  std::string names[] = {name, "debug.exec3.missing." + std::to_string(GetCurrentProcessId())};
  std::string values[] = {"stale", "sentinel"};
  gesher::properties::GetAll(names, values, 2);
  uint32_t serial = 0;
  return values[0] == "new" && values[1].empty() &&
         gesher::properties::Get(name, &value, &serial) && value == "new" && serial != old_serial;
}
bool ChildReadsBootValue(const std::string& name) {
  std::vector<wchar_t> path(32768);
  DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!length || length >= path.size()) return false;
  std::wstring command = L"\"" + std::wstring(path.data(), length) + L"\" --child ";
  command.append(name.begin(), name.end());  // the generated property name is ASCII
  std::vector<wchar_t> writable(command.begin(), command.end());
  writable.push_back(0);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(path.data(), writable.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &startup, &process)) return false;
  DWORD status = 1;
  bool complete = WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0;
  if (complete) GetExitCodeProcess(process.hProcess, &status);
  else {
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 5000);
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return complete && status == 0;
}
}
int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--idle-logger") == 0) return IdleLogger(argv[2]);
  if (argc == 2 && std::strcmp(argv[1], "--clear-log") == 0) {
    std::string error;
    return gesher::logging::Clear(&error) ? 0 : 1;
  }
  if (argc == 3 && std::strcmp(argv[1], "--publish") == 0) {
    return gesher::properties::Set(argv[2], "new") ? 0 : 1;
  }
  if (argc == 3 && std::strcmp(argv[1], "--child") == 0) {
    std::string value;
    return gesher::properties::Initialize() && gesher::properties::Get(argv[2], &value) &&
                   value == "boot-value" ? 0 : 1;
  }
  unsigned failed = 0, checks = 0;
  auto check = [&](const char* label, bool result) {
    ++checks;
    if (!result) { ++failed; std::printf("FAIL property host: %s\n", label); }
  };
  const char* data = std::getenv("ANDROID_DATA");
  if (!data || !*data) { std::puts("property host probe requires ANDROID_DATA"); return 1; }
  std::string original = data;
  check("initialize boot area", gesher::properties::Initialize());
  std::string name = "debug.exec3.host." + std::to_string(GetCurrentProcessId());
  check("publish boot value", gesher::properties::Set(name, "boot-value"));
  std::string scratch = original + "/exec3-scratch-" + std::to_string(GetCurrentProcessId());
  check("create isolated scratch", CreateDirectoryA(scratch.c_str(), nullptr));
  _putenv_s("ANDROID_DATA", scratch.c_str());
  std::string value;
  check("scratch data does not switch property service", gesher::properties::Get(name, &value) && value == "boot-value");
  check("child inherits boot area despite scratch data", ChildReadsBootValue(name));
  check("log default 1 preserves INFO fallback", !gesher::logging::IsLoggable(2, name.c_str(), name.size(), 4, 1) &&
        gesher::logging::IsLoggable(4, name.c_str(), name.size(), 4, 1));
  _putenv_s("ANDROID_DATA", original.c_str());
  check("property reads left scratch empty", RemoveDirectoryA(scratch.c_str()));
  android::add_sysprop_change_callback(Third, 10);
  android::add_sysprop_change_callback(Second, 20);
  android::add_sysprop_change_callback(First, 30);
  gesher::properties::Set(name, "ordinary-write");
  check("ordinary set does not fabricate a report", observed.empty());
  android::report_sysprop_change();
  check("priority and unlocked snapshot dispatch", observed == std::vector<int>({30, 20, 10}));
  observed.clear();
  android::report_sysprop_change();
  check("new callback joins the next report", observed == std::vector<int>({40, 30, 20, 10}));
  check("cross-process cached generation and batched reads", RefreshAcrossProcesses(name));
  check("structured log delete, recreate, rotate, clear, rollover and long paths", LogRegressions());
  std::printf("exec3 property host: %u checks, %u failed\n", checks, failed);
  return failed ? 1 : 0;
}
