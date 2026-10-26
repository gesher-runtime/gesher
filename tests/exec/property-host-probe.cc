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
  std::printf("exec3 property host: %u checks, %u failed\n", checks, failed);
  return failed ? 1 : 0;
}
