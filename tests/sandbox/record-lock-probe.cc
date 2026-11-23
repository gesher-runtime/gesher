// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include "services_zygote.h"

namespace {
namespace launch = gesher::zygote;
namespace fs = std::filesystem;
constexpr DWORD kSharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr wchar_t kOwner[] = L"owner";
constexpr wchar_t kObserver[] = L"observer";
static_assert(sizeof(flock) == 32);
std::u16string U16(const std::wstring& value) { return {value.begin(), value.end()}; }
std::u16string Quote(const fs::path& path) { return u"\"" + U16(path.native()) + u"\""; }

bool Check(bool passed, const char* description) {
  std::printf("%s: %s (errno=%d windows=%lu)\n", description, passed ? "ok" : "FAILED",
              errno, GetLastError());
  std::fflush(stdout);
  return passed;
}
bool Contains(const fs::path& log, const char* marker) {
  std::ifstream source(log);
  std::string line;
  while (std::getline(source, line)) if (line == marker) return true;
  return false;
}
bool WaitMarker(int pid, const fs::path& log, const char* marker) {
  const auto end = GetTickCount64() + 10000;
  do {
    if (Contains(log, marker)) return true;
    if (launch::ExitCode(pid) != STILL_ACTIVE) break;
    Sleep(20);
  } while (GetTickCount64() < end);
  return Contains(log, marker);
}
bool WaitExit(int pid) {
  const auto end = GetTickCount64() + 10000;
  do {
    const int status = launch::ExitCode(pid);
    if (status != STILL_ACTIVE) return status == 0;
    Sleep(20);
  } while (GetTickCount64() < end);
  return false;
}
bool Lowbox() {
  HANDLE token = nullptr;
  DWORD is_lowbox = 0;
  DWORD size = 0;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  const BOOL okay = GetTokenInformation(token, TokenIsAppContainer, &is_lowbox, sizeof(is_lowbox), &size);
  CloseHandle(token);
  return okay && is_lowbox;
}
bool Phase(const fs::path& path, char* phase, bool set) {
  HANDLE file = CreateFileW(path.c_str(), set ? GENERIC_WRITE : GENERIC_READ, kSharing,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD bytes = 0;
  const BOOL okay = set ? WriteFile(file, phase, 1, &bytes, nullptr) : ReadFile(file, phase, 1, &bytes, nullptr);
  CloseHandle(file);
  return okay && bytes == 1;
}
bool SendPhase(const fs::path& dir, char phase) { return Phase(dir / L"phase", &phase, true); }
bool WaitPhase(const fs::path& dir, char expected) {
  const auto end = GetTickCount64() + 10000;
  do {
    char phase = 0;
    if (Phase(dir / L"phase", &phase, false) && phase == expected) return true;
    Sleep(20);
  } while (GetTickCount64() < end);
  return false;
}
int OpenData(const fs::path& dir) {
  return _wsopen((dir / L"shared").c_str(), _O_RDWR | _O_BINARY, _SH_DENYNO);
}
flock Range(short kind) { return {kind, SEEK_SET, 4, 8, 0}; }
void Mark(const char* text) { std::puts(text); std::fflush(stdout); }

int Hold(const fs::path& dir) {
  if (!Check(Lowbox(), "holder executes in an AppContainer")) return 1;
  HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\Gesher.RecordLocks.v1");
  if (mutex) CloseHandle(mutex);
  if (!Check(!mutex, "guest cannot open host registry mutex")) return 1;
  const int first = OpenData(dir);
  const int other = OpenData(dir);
  auto lock = Range(F_WRLCK);
  if (!Check(first >= 0 && other >= 0, "holder opens two descriptions")) return 1;
  if (!Check(fcntl(first, F_SETLK, &lock) == 0, "holder acquires POSIX range")) return 1;
  Mark("POSIX held");
  if (!WaitPhase(dir, '1') || !Check(close(other) == 0, "independent close releases POSIX owner")) return 1;
  Mark("POSIX released");
  if (!WaitPhase(dir, '2') || !Check(fcntl(first, F_OFD_SETLK, &lock) == 0, "holder acquires OFD range")) return 1;
  const int alias = dup(first);
  if (!Check(alias >= 0 && close(first) == 0, "OFD survives close with an alias")) return 1;
  Mark("OFD held");
  if (!WaitPhase(dir, '3') || !Check(close(alias) == 0, "last alias closes")) return 1;
  Mark("OFD released");
  return WaitPhase(dir, '4') ? 0 : 1;
}

int Observe(const fs::path& dir, int pid) {
  if (!Check(Lowbox(), "observer executes in another AppContainer")) return 1;
  const int fd = OpenData(dir);
  if (!Check(fd >= 0, "observer opens same real file")) return 1;
  auto conflict = Range(F_WRLCK);
  if (!Check(fcntl(fd, F_GETLK, &conflict) == 0, "observer queries through native fcntl")) return 1;
  if (pid) {
    auto attempt = Range(F_WRLCK);
    std::printf("conflict type %d range %lld:%lld owner %lld expected %d\n",
                conflict.l_type, static_cast<long long>(conflict.l_start), static_cast<long long>(conflict.l_len),
                static_cast<long long>(conflict.l_pid), pid);
    if (!Check(conflict.l_type == F_WRLCK && conflict.l_start == 4 && conflict.l_len == 8 &&
               conflict.l_pid == pid && fcntl(fd, F_SETLK, &attempt) == -1 &&
               (errno == EAGAIN || errno == EACCES), "observer sees actual conflict, range and owner")) return 1;
  } else if (!Check(conflict.l_type == F_UNLCK, "observer sees released lock while holder remains alive")) {
    return 1;
  }
  return close(fd) == 0 ? 0 : 1;
}
std::u16string Profile(const fs::path& dir, const wchar_t* suffix) {
  return U16(dir.filename().native()) + u"." + U16(suffix);
}

int Server(const fs::path& dir) {
  if (!Check(launch::HasProtectedLaunchAuthority(), "server has real startup authority")) return 1;
  const auto self = launch::ExePath();
  const fs::path exe(self);
  for (const auto* role : {kOwner, kObserver}) {
    const auto profile = Profile(dir, role);
    if (!Check(launch::EnsureAppContainer(profile, profile) == ERROR_SUCCESS &&
               launch::GrantToAppContainer(self, profile, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false) == 0 &&
               launch::GrantToAppContainer(U16((dir / L"shared").native()), profile,
                                          FILE_GENERIC_READ | FILE_GENERIC_WRITE, false) == 0 &&
               launch::GrantToAppContainer(U16((dir / L"phase").native()), profile, FILE_GENERIC_READ, false) == 0,
               "grant exact executable and fixture data")) return 1;
  }
  std::array<uint8_t, 16> owner_id{}, observer_id{};
  owner_id[0] = 1;
  observer_id[0] = 2;
  const auto owner = Profile(dir, kOwner);
  const auto observer = Profile(dir, kObserver);
  const auto owner_log = dir / L"owner.log";
  const auto start = [&](const std::u16string& args, const fs::path& log, const std::u16string& profile,
                         uint32_t uid, const std::array<uint8_t, 16>& id) {
    return launch::SpawnProtectedApp(Quote(exe) + args + u" " + Quote(dir), {}, {},
                                     U16(log.native()), {profile, profile, {u"gesherBinderClient"}}, uid, id);
  };
  const int holder = start(u" --hold", owner_log, owner, 11001, owner_id);
  int check_number = 0;
  const auto observe = [&](int expected) {
    const auto log = dir / (L"observer" + std::to_wstring(++check_number) + L".log");
    const int process = start(u" --observe " + U16(std::to_wstring(expected)), log, observer, 11002, observer_id);
    return process > 0 && WaitExit(process);
  };
  return Check(holder > 0 && WaitMarker(holder, owner_log, "POSIX held") && observe(holder) &&
               SendPhase(dir, '1') && WaitMarker(holder, owner_log, "POSIX released") && observe(0) &&
               SendPhase(dir, '2') && WaitMarker(holder, owner_log, "OFD held") && observe(-1) &&
               SendPhase(dir, '3') && WaitMarker(holder, owner_log, "OFD released") && observe(0) &&
               SendPhase(dir, '4') && WaitExit(holder), "protected cross-process record locks") ? 0 : 1;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wstring(argv[1]) == L"--server") return Server(argv[2]);
  if (argc == 3 && std::wstring(argv[1]) == L"--hold") return Hold(argv[2]);
  if (argc == 4 && std::wstring(argv[1]) == L"--observe") return Observe(argv[3], _wtoi(argv[2]));
  if (argc != 3) {
    std::puts("Usage: appcontainer_record_lock_probe <runtime dir> <scratch parent>");
    return 2;
  }
  const fs::path runtime = fs::absolute(argv[1]);
  const auto id = L"gesher-record-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
  const auto dir = fs::absolute(argv[2]) / id;
  fs::create_directories(dir);
  {
    std::ofstream(dir / L"phase", std::ios::binary) << '0';
    std::ofstream(dir / L"shared", std::ios::binary) << "record-lock sandbox fixture";
  }
  const fs::path self(launch::ExePath());
  const auto probe = dir / self.filename();
  fs::copy_file(self, probe);
  const auto device = U16(id);
  const auto pids = launch::StartProtectedDevice(device,
      {Quote(runtime / L"binderd.exe"), Quote(runtime / L"servicemanager.exe"), Quote(probe) + u" --server " + Quote(dir)},
      {U16((dir / L"binder.log").native()), U16((dir / L"manager.log").native()), U16((dir / L"server.log").native())},
      {u"GESHER_SANDBOX=appcontainer", u"GESHER_BINDER=" + device});
  bool ok = Check(pids.size() == 3 && WaitExit(pids[2]), "protected server drives fixture");
  if (!ok && pids.size() == 3) launch::KillChild(pids[2], launch::ChildStartTime(pids[2]));
  for (const auto* role : {kOwner, kObserver}) launch::DeleteAppContainer(Profile(dir, role));
  std::wprintf(L"Fixture logs: %ls\n", dir.c_str());
  return ok ? 0 : 1;
}
