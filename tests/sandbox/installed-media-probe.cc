// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <windows.h>
#include <dirent.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <unistd.h>
#include <gesher/storage_policy.h>
#include <gesher/binder_endpoint.h>
#include <gesher/rooted_storage_wire.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <sys/stat.h>
#include "services_zygote.h"

namespace {
namespace fs = std::filesystem;
namespace launch = gesher::zygote;
namespace wire = gesher::rooted_storage_wire;
constexpr char kPackage[] = "gesher.fixture.media";
constexpr char kInstallation[] = "00000000-0000-0003-0000-000000000000";
constexpr wchar_t kPackagePath[] = L"gesher.fixture.media";
std::u16string U16(const std::wstring& value) { return {value.begin(), value.end()}; }
std::u16string Quote(const fs::path& path) { return u"\"" + U16(path.native()) + u"\""; }
bool Check(bool passed, const char* message) {
  std::cout << (passed ? "ok " : "FAIL ") << message << " errno=" << errno
            << " win32=" << GetLastError() << std::endl;
  return passed;
}
bool Read(const std::string& path, char expected) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  char b = 0;
  const bool result = read(fd, &b, 1) == 1 && b == expected;
  close(fd);
  return result;
}
bool Denied(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd == -1) return errno == EACCES;
  close(fd);
  return false;
}
bool Write(const std::string& path) {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
  if (fd == -1) return false;
  const bool result = write(fd, "x", 1) == 1;
  close(fd);
  return result;
}
bool List(const std::string& path, const char* expected) {
  DIR* dir = opendir(path.c_str());
  if (!dir) return false;
  bool found = false;
  while (const dirent* next = readdir(dir)) {
    found |= std::string_view(next->d_name) == expected;
  }
  closedir(dir);
  return found;
}

bool CheckSqliteAccess(const std::string& path, bool expected_write) {
  auto* vfs = sqlite3_vfs_find(nullptr);
  char name[32768]{};
  int readable = 0, writable = -1;
  return vfs && vfs->xFullPathname(vfs, path.c_str(), sizeof(name), name) == SQLITE_OK &&
      vfs->xAccess(vfs, name, SQLITE_ACCESS_READ, &readable) == SQLITE_OK && readable == 1 &&
      vfs->xAccess(vfs, name, SQLITE_ACCESS_READWRITE, &writable) == SQLITE_OK &&
      writable == static_cast<int>(expected_write);
}

template <typename Predicate>
bool WaitFor(Predicate ready) {
  const ULONGLONG deadline = GetTickCount64() + 10000;
  do {
    if (ready()) return true;
    Sleep(10);
  } while (GetTickCount64() < deadline);
  return false;
}

bool LogContains(const fs::path& path, std::string_view marker) {
  std::ifstream input(path);
  for (std::string line; std::getline(input, line);) {
    if (line == marker) return true;
  }
  return false;
}

bool CheckLivePolicy(const std::string& photo) {
  const int held = open(photo.c_str(), O_RDONLY | O_CLOEXEC);
  if (held < 0) return false;
  std::cout << "ready to revoke image grant" << std::endl;
  bool ok = Check(WaitFor([&] { return Denied(photo); }), "fresh opens observe live downgrade");
  char byte = 0;
  ok &= Check(read(held, &byte, 1) == 1 && byte == 'P', "issued file handle retains its original access");
  close(held);
  std::cout << "ready to restore image grant" << std::endl;
  ok &= Check(WaitFor([&] { return Read(photo, 'P'); }), "fresh opens observe restored grant");
  return ok;
}

int Guest(const fs::path& dir, const std::wstring& mode) {
  gesher::InitializeExternalStoragePolicy();
  gesher::process_identity::Process process;
  if (!Check(gesher::process_identity::ReadLiveProcess(GetCurrentProcess(), &process) &&
      process.principal.container.size != 0 && gesher::binder_endpoint::Current().IsGuest(),
      "actual installed lowbox private Binder admission")) return 1;
  const auto media = dir / L"media" / L"0";
  const std::string root = media.generic_string();
  const std::string photo = root + "/DCIM/photo.jpg";
  const std::string movie = root + "/DCIM/movie.mp4";
  const std::string secret = root + "/Android/data/other.app/secret.jpg";
  const std::string own = root + "/Android/data/" + kPackage + "/own.txt";
  const auto runtime_data = dir / L"runtime-data";
  const HANDLE safe_raw = CreateFileW(runtime_data.c_str(), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  bool ok = Check(safe_raw != INVALID_HANDLE_VALUE, "kernel grants a host-approved runtime file");
  if (safe_raw != INVALID_HANDLE_VALUE) CloseHandle(safe_raw);
  ok &= Check(Read(runtime_data.generic_string(), 'R'), "POSIX reads a host-approved runtime file");
  const HANDLE raw = CreateFileW((media / L"DCIM" / L"photo.jpg").c_str(), GENERIC_READ,
      FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (raw != INVALID_HANDLE_VALUE) CloseHandle(raw);
  ok &= Check(raw == INVALID_HANDLE_VALUE, "no raw native ACL grant for shared media");
  if (mode == L"stale") return !(ok && Check(Denied(photo) && Denied(own), "stale installation cannot use snapshot"));
  ok &= Check(Denied(secret) && Read(own, 'O'), "other private rejected and own external allowed");
  wire::Request request;
  request.operation = wire::Op::kOpen;
  request.source_area = wire::kInstalledMedia;
  std::wstring escaping = L"DCIM/../DCIM/photo.jpg";
  request.path_units = escaping.size();
  wire::Reply reply;
  ok &= Check(gesher::binder_endpoint::InstalledMedia(request, escaping, L"", &reply) != 0 &&
      reply.handle == 0, "wire relative traversal refused");
  if (mode == L"media") {
    ok &= Check(Read(photo, 'P') && Denied(movie), "exact installed grant allows images only");
    ok &= Check(List(root + "/DCIM", "photo.jpg") && !List(root + "/Android/data", "other.app"), "shared directory traversal only");
    struct stat info{};
    ok &= Check(stat(photo.c_str(), &info) == 0 && info.st_size == 1 &&
        access(photo.c_str(), R_OK) == 0 && access(photo.c_str(), W_OK) == -1,
        "metadata and access follow host media authorization");
    ok &= Check(CheckSqliteAccess(photo, false) && CheckSqliteAccess(root + "/DCIM", false),
        "SQLite reports the real read-only media grant for a file and directory");
    const int fd = open((root + "/DCIM").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const int child = fd < 0 ? -1 : openat(fd, "photo.jpg", O_RDONLY | O_CLOEXEC);
    if (child >= 0) close(child);
    if (fd >= 0) close(fd);
    ok &= Check(child >= 0, "relative directory descriptor opens right file");
    ok &= Check(!Write(photo), "image permission does not grant write");
    std::array<bool, 8> simultaneous{};
    std::array<std::thread, simultaneous.size()> requests;
    for (size_t index = 0; index < requests.size(); ++index) {
      requests[index] = std::thread([&, index] {
        simultaneous[index] = Read(photo, 'P') && List(root + "/DCIM", "photo.jpg");
      });
    }
    for (auto& request : requests) request.join();
    bool completed = true;
    for (const bool success : simultaneous) completed &= success;
    ok &= Check(completed, "simultaneous authorised opens and directory cursors complete");
    ok &= CheckLivePolicy(photo);
  } else if (mode == L"scoped") {
    ok &= Check(Denied(photo) && Denied(movie), "scoped grants are mediated through providers");
  } else {
    const auto made = root + "/new";
    const auto first = made + "/first.txt";
    const auto second = made + "/second.txt";
    ok &= Check(CheckSqliteAccess(photo, true) && CheckSqliteAccess(root + "/DCIM", true),
        "SQLite reports the authenticated manager write grant for a file and directory");
    ok &= Check(Read(movie, 'V') && mkdir(made.c_str(), 0755) == 0 && Write(first) &&
        rename(first.c_str(), second.c_str()) == 0 && Read(second, 'x') && unlink(second.c_str()) == 0 &&
        rmdir(made.c_str()) == 0, "manager reads and writes through actual rooted file operations");
  }
  return !ok;
}

int Wait(int pid) {
  if (pid <= 0) return pid;
  const ULONGLONG deadline = GetTickCount64() + 10000;
  while (GetTickCount64() < deadline) {
    const int exit = launch::ExitCode(pid);
    if (exit != STILL_ACTIVE) return exit;
    Sleep(10);
  }
  launch::KillChild(pid, launch::ChildStartTime(pid));
  return -1;
}

bool Publish(const fs::path& snapshot, const char* mode, const char* installation) {
  auto staged = snapshot;
  staged += L".tmp";
  {
    std::ofstream output(staged, std::ios::binary | std::ios::trunc);
    output << "v=1\npkg=" << kPackage << "\nuid=11007\ninstallation=" << installation
           << "\nmode=" << mode << "\nmedia_mask=1\n";
    output.close();
    if (!output) return false;
  }
  return MoveFileExW(staged.c_str(), snapshot.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

int Server(const fs::path& dir) {
  if (!launch::HasProtectedLaunchAuthority()) return 1;
  const auto exe = fs::path(launch::ExePath());
  const std::u16string profile = U16(dir.filename().native());
  if (launch::EnsureAppContainer(profile, profile) ||
      launch::GrantToAppContainer(U16(exe.native()), profile, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false) ||
      launch::GrantToAppContainer(U16((dir / L"runtime-data").native()), profile, FILE_GENERIC_READ, false)) return 1;
  bool ok = true;
  uint64_t sequence = 0;
  const auto snapshot = dir / L"system" / L"storage-policy" / kPackagePath;
  for (const wchar_t* operation : {L"media", L"manager", L"scoped", L"stale"}) {
    const std::wstring_view mode(operation);
    const char* access = mode == L"manager" ? "MANAGER" : mode == L"scoped" ? "SCOPED" : "MEDIA_RO";
    if (!Publish(snapshot, access, mode == L"stale" ? "stale" : kInstallation)) {
      ok = false;
      break;
    }
    const auto log = dir / (std::wstring(operation) + L".log");
    std::array<uint8_t, 16> installation{};
    installation[0] = 3;
    const int pid = launch::SpawnProtectedApp(Quote(exe) + u" --guest " + Quote(dir) + u" " + U16(operation),
        {u"GESHER_APP_PACKAGE=gesher.fixture.media", u"GESHER_UID=11007"}, {},
        U16(log.native()),
        {profile, U16(kPackagePath), {u"gesherBinderClient"}}, 11007, installation, ++sequence, 0, false);
    if (mode == L"media" && pid > 0) {
      ok &= Check(WaitFor([&] { return LogContains(log, "ready to revoke image grant"); }) &&
          Publish(snapshot, "SCOPED", kInstallation), "publish an atomic downgrade while the guest runs");
      ok &= Check(WaitFor([&] { return LogContains(log, "ready to restore image grant"); }) &&
          Publish(snapshot, "MEDIA_RO", kInstallation), "restore the same installation's live grants");
    }
    ok &= Check(pid > 0 && Wait(pid) == 0, "guest scenario");
  }
  launch::DeleteAppContainer(profile);
  return !ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 4 && std::wstring_view(argv[1]) == L"--guest") return Guest(argv[2], argv[3]);
  if (argc == 3 && std::wstring_view(argv[1]) == L"--server") return Server(argv[2]);
  if (argc != 3) {
    std::puts("Usage: appcontainer_installed_media_probe <runtime dir> <scratch parent>");
    return 2;
  }
  const fs::path runtime = fs::absolute(argv[1]);
  const auto device = L"media-qa-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
  const fs::path dir = fs::absolute(argv[2]) / device;
  const auto private_dir = dir / L"media" / L"0" / L"Android" / L"data";
  fs::create_directories(private_dir / L"other.app");
  fs::create_directories(private_dir / kPackagePath);
  fs::create_directories(dir / L"media" / L"0" / L"DCIM");
  fs::create_directories(dir / L"system" / L"storage-policy");
  std::ofstream(dir / L"media" / L"0" / L"DCIM" / L"photo.jpg", std::ios::binary) << 'P';
  std::ofstream(dir / L"media" / L"0" / L"DCIM" / L"movie.mp4", std::ios::binary) << 'V';
  std::ofstream(private_dir / L"other.app" / L"secret.jpg", std::ios::binary) << 'S';
  std::ofstream(private_dir / kPackagePath / L"own.txt", std::ios::binary) << 'O';
  std::ofstream(dir / L"runtime-data", std::ios::binary) << 'R';
  const fs::path probe = dir / L"media-probe.exe";
  fs::copy_file(fs::path(launch::ExePath()), probe);
  const auto pids = launch::StartProtectedDevice(U16(device),
      {Quote(runtime / L"binderd.exe"), Quote(runtime / L"servicemanager.exe"), Quote(probe) + u" --server " + Quote(dir)},
      {U16((dir / L"binder.log").native()), U16((dir / L"manager.log").native()), U16((dir / L"server.log").native())},
      {u"GESHER_BINDER=" + U16(device), u"GESHER_SANDBOX=appcontainer-trusted",
       u"ANDROID_DATA=" + U16(dir.native()), u"GESHER_ANDROID_DATA_HOST_PATH=" + U16(dir.native())});
  std::wcout << dir << std::endl;
  if (pids.size() != 3 || Wait(pids[2]) != 0) {
    for (const auto* log : {L"server.log", L"media.log", L"manager.log", L"scoped.log", L"stale.log"}) {
      std::ifstream input(dir / log);
      if (input.peek() != std::ifstream::traits_type::eof()) std::cout << input.rdbuf();
    }
    return 1;
  }
  std::puts("installed media probe all passed");
  return 0;
}
