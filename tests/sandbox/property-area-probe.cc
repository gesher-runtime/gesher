// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include <gesher/environment.h>
#include <gesher/system_properties.h>
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "app_container.h"
#include "services_zygote.h"

namespace {
namespace ac = gesher::appcontainer;
namespace props = gesher::properties;
constexpr char kName[] = "debug.gesher.property_probe";
constexpr char kPersistent[] = "persist.gesher.property_probe";
std::u16string U16(const std::wstring& value) { return {value.begin(), value.end()}; }

bool Check(bool result, const char* description) {
  std::printf("%s: %s\n", description, result ? "ok" : "FAILED");
  std::fflush(stdout);
  return result;
}

int ReadGuest(const wchar_t* path, const wchar_t* persistent) {
  bool ok = Check(props::ReadOnlyAreaPath().empty(), "guest cannot prepare an area");
  std::string value;
  uint32_t serial = 0;
  ok &= Check(props::Get(kName, &value, &serial) && value == "before", "guest reads broker value");
  ok &= Check(!props::Set(kName, "from-guest") && !props::Set(kPersistent, "from-guest"),
              "guest property writes denied");
  HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ok &= Check(file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED,
              "kernel denies writable area file");
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ok &= Check(file != INVALID_HANDLE_VALUE, "kernel permits read-only area file");
  if (file != INVALID_HANDLE_VALUE) {
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    ok &= Check(mapping == nullptr && GetLastError() == ERROR_ACCESS_DENIED,
                "kernel denies writable mapping from read-only file");
    if (mapping) CloseHandle(mapping);
    CloseHandle(file);
  }
  file = CreateFileW(persistent, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ok &= Check(file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED,
              "kernel denies direct persistent storage read");
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  if (!ok) return 1;

  std::puts("guest waiting");
  std::fflush(stdout);
  const std::string key = kName;
  const props::WaitTimeout timeout{10, 0};
  uint32_t after = serial;
  return Check(props::Wait(&key, serial, &after, &timeout) && after != serial &&
               props::Get(kName, &value) && value == "after", "guest observes coherent live update") ? 0 : 1;
}

int RunBroker(const std::wstring& root) {
  std::wstring profile = L"Gesher.PropertyProbe." + std::to_wstring(GetCurrentProcessId()) +
                         L"." + std::to_wstring(GetTickCount64());
  ac::Sid sid;
  if (!Check(ac::EnsureProfile(profile, profile, &sid) == ERROR_SUCCESS, "create test profile")) return 1;
  struct ProfileCleanup {
    const std::wstring& name;
    ~ProfileCleanup() { ac::DeleteContainerProfile(name); }
  } cleanup{profile};

  wchar_t exe[MAX_PATH];
  DWORD size = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring guest = root + L"\\guest.exe";
  if (!Check(size && size < MAX_PATH && CopyFileW(exe, guest.c_str(), FALSE) &&
             ac::Grant(guest, sid.value, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false) == ERROR_SUCCESS,
             "prepare fixture executable")) return 1;
  if (!Check(gesher::SetWideEnvironment(L"ANDROID_DATA", root.c_str()) == 0 &&
             gesher::SetWideEnvironment(L"GESHER_BINDER", profile.c_str()) == 0 &&
             props::Set(kName, "before") && props::Set(kPersistent, "kept"),
             "initialize broker area")) return 1;
  std::wstring path = props::ReadOnlyAreaPath();
  std::wstring persistent = root + L"\\property\\persistent_properties";
  if (!Check(!path.empty() && ac::Grant(path, sid.value, FILE_GENERIC_READ, false) == ERROR_SUCCESS,
             "grant exact area read")) return 1;
  std::wstring log = root + L"\\guest.log";
  auto command = L"\"" + guest + L"\" --guest \"" + path + L"\" \"" + persistent + L"\"";
  const gesher::zygote::AppContainerSpec spec{U16(profile), U16(profile), {}};
  int pid = gesher::zygote::SpawnInAppContainer(U16(command), {}, {}, U16(log), spec);
  if (!Check(pid > 0, "start real contained guest")) return 1;

  char output[4096]{};
  auto refresh_log = [&] {
    HANDLE file = CreateFileW(log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD got = 0;
    if (ReadFile(file, output, sizeof(output) - 1, &got, nullptr)) output[got] = '\0';
    CloseHandle(file);
  };
  int exit_code = STILL_ACTIVE;
  for (int i = 0; i < 400 && exit_code == STILL_ACTIVE; ++i) {
    refresh_log();
    if (std::string_view(output).find("guest waiting") != std::string_view::npos) break;
    Sleep(25);
    exit_code = gesher::zygote::ExitCode(pid);
  }
  bool ok = Check(std::string_view(output).find("guest waiting") != std::string_view::npos &&
                  props::Set(kName, "after"), "broker writes while guest waits");
  for (int i = 0; i < 400 && exit_code == STILL_ACTIVE; ++i) {
    Sleep(25);
    exit_code = gesher::zygote::ExitCode(pid);
  }
  if (exit_code == STILL_ACTIVE) gesher::zygote::Kill(pid);
  refresh_log();
  std::fputs(output, stdout);
  ok &= Check(exit_code == 0, "guest verifies read-only live view");
  std::string value;
  ok &= Check(props::Get(kPersistent, &value) && value == "kept", "persistent value unchanged");
  if (ok) std::puts("property-area probe all passed");
  return ok ? 0 : 1;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 4 && std::wstring_view(argv[1]) == L"--guest") return ReadGuest(argv[2], argv[3]);
  if (argc != 2) return 2;
  std::filesystem::path root = std::filesystem::absolute(argv[1]);
  return std::filesystem::create_directories(root) ? RunBroker(root.native()) : 2;
}
