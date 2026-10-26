// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Native regressions for the real host libutils callback path and boot area.
#include <gesher/logging.h>
#include <gesher/system_properties.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
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
}
int main() {
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
