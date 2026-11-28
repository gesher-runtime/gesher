// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <windows.h>
#include <dirent.h>
#include <fcntl.h>
#include <gesher/binder_endpoint.h>
#include <gesher/bionic_shim.h>
#include <gesher/cohost_bootstrap.h>
#include <io.h>
#include <poll.h>
#include <share.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include "exec_spawn.h"
#include "services_zygote.h"
#include "cohost_permission_server.h"
#include "rooted_storage_server.h"
#include "../../compat/bionic/abi_types.h"

namespace {
namespace launch = gesher::zygote;
namespace fs = std::filesystem;
constexpr DWORD kSharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr wchar_t kOwner[] = L"owner";
constexpr wchar_t kObserver[] = L"observer";
constexpr wchar_t kStandalone[] = L"standalone";
constexpr char kPipeData[] = "contained pipe transfer";
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
  if (!WaitPhase(dir, '4')) return 1;
  const int exiting = OpenData(dir);
  if (!Check(exiting >= 0 && fcntl(exiting, F_OFD_SETLK, &lock) == 0, "exiting holder leaves a live OFD")) return 1;
  Mark("exiting OFD held");
  if (!WaitPhase(dir, '5')) return 1;
  ExitProcess(0);  // Exit without calling any descriptor close wrapper.
}

int Observe(const fs::path& dir, int pid) {
  if (!Check(Lowbox(), "observer executes in an AppContainer")) return 1;
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

bool CheckForkBinder() {
  if (!Lowbox() || gesher::binder_bootstrap::GuestUid() != 11001) return false;
  const int fd = open("/dev/binder", O_RDWR | O_CLOEXEC);
  return fd >= 0 && close(fd) == 0;
}

int ForkExecGuest(const fs::path& dir) {
  using Fork = int (BIONIC_SYSV*)(int, int, const void*);
  using Exec = int (BIONIC_SYSV*)(const char*, char* const*);
  using Wait = int (BIONIC_SYSV*)(int, int*, int);
  const auto fork = reinterpret_cast<Fork>(gesher::bionic::LookupBionicSymbol("__gesher_fork"));
  const auto exec = reinterpret_cast<Exec>(gesher::bionic::LookupBionicSymbol("execv"));
  const auto wait = reinterpret_cast<Wait>(gesher::bionic::LookupBionicSymbol("waitpid"));
  if (!Check(fork && exec && wait && CheckForkBinder(), "contained fork parent has its bound Binder identity")) return 1;
  if (!Check(SetCurrentDirectoryW(dir.c_str()), "contained fork uses its accessible fixture directory")) return 1;
  const auto path = fs::path(launch::ExePath()).native();
  auto command = L"\"" + path + L"\"";
  // This stays suspended and inherits our actual package and device job. It
  // is a sibling of the caller below, rather than that caller's own child.
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION sibling{};
  if (!Check(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE,
                 CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &sibling),
             "start the same-package sibling denial control suspended")) return 1;
  auto program = gesher::exec::Narrow(path);
  char mode[] = "--fork-exec-binder";
  char* args[] = {program.data(), mode, nullptr};
  const int pid = fork(17, 0, nullptr);
  if (pid == 0) {
    if (!Check(CheckForkBinder(), "contained fork child immediately uses its own bound Binder identity") ||
        !Check(!gesher::binder_endpoint::EnrollChild(sibling.hProcess), "fork cannot enroll its same-package sibling")) {
      TerminateProcess(GetCurrentProcess(), 20);
    }
    CloseHandle(sibling.hProcess);
    CloseHandle(sibling.hThread);
    exec(program.c_str(), args);
    std::fprintf(stderr, "protected fork: execv failed with errno=%d\n", errno);
    TerminateProcess(GetCurrentProcess(), 21);
  }
  int status = -1;
  const bool complete = pid > 0 && wait(pid, &status, 0) == pid && status == 0;
  TerminateProcess(sibling.hProcess, 0);
  WaitForSingleObject(sibling.hProcess, 1000);
  CloseHandle(sibling.hThread);
  CloseHandle(sibling.hProcess);
  return Check(complete, "forked child and real exec replacement preserve Binder admission") ? 0 : 1;
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
  if (!Check(launch::GrantToAppContainer(U16(dir.native()), owner, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false) == 0,
             "grant the fork fixture's exact current directory")) return 1;
  const auto owner_log = dir / L"owner.log";
  uint64_t start_sequence = 0;
  const auto start = [&](const std::u16string& args, const fs::path& log, const std::u16string& profile,
                         uint32_t uid, const std::array<uint8_t, 16>& id) {
    const std::u16string package = uid == 11001 ? u"gesher.fixture.record.owner" : u"gesher.fixture.record.observer";
    return launch::SpawnProtectedApp(Quote(exe) + args + u" " + Quote(dir), {}, {},
                                    U16(log.native()), {profile, package, {u"gesherBinderClient"}},
                                    uid, id, ++start_sequence, 0, false);
  };
  const int fork = start(u" --fork-exec", dir / L"fork-exec.log", owner, 11001, owner_id);
  if (!Check(fork > 0 && WaitExit(fork), "protected fork and exec use the exact Binder enrollment")) return 1;
  const int holder = start(u" --hold", owner_log, owner, 11001, owner_id);
  int check_number = 0;
  const auto observe = [&](int expected) {
    const auto log = dir / (L"observer" + std::to_wstring(++check_number) + L".log");
    const int process = start(u" --observe " + U16(std::to_wstring(expected)), log, observer, 11002, observer_id);
    return process > 0 && WaitExit(process);
  };
  const auto shared = dir / L"shared";
  const auto release_after_exit = [&] {
    if (DeleteFileW(shared.c_str()) || GetLastError() != ERROR_SHARING_VIOLATION) return false;
    if (!SendPhase(dir, '5') || !WaitExit(holder)) return false;
    const auto end = GetTickCount64() + 5000;
    do {
      // Do not call fcntl to induce opportunistic cleanup. Verify the actual
      // Windows share reservation leaves with the last guest handle.
      if (DeleteFileW(shared.c_str())) return true;
      if (GetLastError() != ERROR_SHARING_VIOLATION) return false;
      Sleep(20);
    } while (GetTickCount64() < end);
    return false;
  };
  return Check(holder > 0 && WaitMarker(holder, owner_log, "POSIX held") && observe(holder) &&
               SendPhase(dir, '1') && WaitMarker(holder, owner_log, "POSIX released") && observe(0) &&
               SendPhase(dir, '2') && WaitMarker(holder, owner_log, "OFD held") && observe(-1) &&
               SendPhase(dir, '3') && WaitMarker(holder, owner_log, "OFD released") && observe(0) &&
               SendPhase(dir, '4') && WaitMarker(holder, owner_log, "exiting OFD held") &&
               Check(release_after_exit(), "broker reclaims the last OFD anchor after unclean guest exit"),
               "protected cross-process record locks") ? 0 : 1;
}

bool StandaloneChild(const std::wstring& arguments, HANDLE input) {
  const fs::path exe(launch::ExePath());
  gesher::exec::Launch child;
  child.application = exe.native();
  gesher::exec::AppendQuoted(&child.command_line, exe.native());
  child.command_line += arguments;
  PROCESS_INFORMATION process{};
  // An explicit child environment does not supply any binder device name.
  const auto environment = gesher::exec::EnvironmentBlock({});
  const DWORD started = gesher::exec::Spawn(child, environment.c_str(), L"", input, GetStdHandle(STD_OUTPUT_HANDLE),
      GetStdHandle(STD_ERROR_HANDLE), &process);
  if (!Check(started == ERROR_SUCCESS, "standalone spawns through the production native path")) return false;
  DWORD status = STILL_ACTIVE;
  const bool passed = WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0 &&
      GetExitCodeProcess(process.hProcess, &status) && status == 0;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return passed;
}

int QueryInheritedFile() {
  auto own = Range(F_WRLCK);
  auto ordinary = own;
  return Check(Lowbox() && fcntl(STDIN_FILENO, F_OFD_GETLK, &own) == 0 && own.l_type == F_UNLCK &&
               fcntl(STDIN_FILENO, F_GETLK, &ordinary) == 0 && ordinary.l_type == F_WRLCK &&
               ordinary.l_pid == -1 && ordinary.l_start == 4 && ordinary.l_len == 8,
               "standalone descendant inherits the exact OFD owner") ? 0 : 1;
}

int ReadInheritedPipe() {
  char data[sizeof(kPipeData)]{};
  return Check(Lowbox() && read(STDIN_FILENO, data, sizeof(data)) == static_cast<ssize_t>(sizeof(data)) &&
               std::string(data, sizeof(data)) == std::string(kPipeData, sizeof(kPipeData)),
               "contained descendant reads the actual inherited pipe payload") ? 0 : 1;
}

bool CheckStandalonePipe() {
  int ends[2] = {-1, -1};
  if (!Check(pipe2(ends, O_CLOEXEC | O_NONBLOCK) == 0, "AppContainer creates a real anonymous pipe")) return false;
  char byte = 0;
  pollfd ready{ends[0], POLLIN, 0};
  const bool transferred = (fcntl(ends[0], F_GETFD) & FD_CLOEXEC) != 0 &&
      (fcntl(ends[1], F_GETFL) & O_NONBLOCK) != 0 &&
      read(ends[0], &byte, 1) == -1 && errno == EAGAIN &&
      write(ends[1], kPipeData, sizeof(kPipeData)) == static_cast<ssize_t>(sizeof(kPipeData)) &&
      poll(&ready, 1, 1000) == 1 && (ready.revents & POLLIN) != 0 &&
      StandaloneChild(L" --read-inherited-pipe", reinterpret_cast<HANDLE>(_get_osfhandle(ends[0])));
  const bool closed_writer = close(ends[1]) == 0;
  const bool eof = read(ends[0], &byte, 1) == 0;
  const bool closed_reader = close(ends[0]) == 0;
  return Check(transferred && closed_writer && eof && closed_reader,
               "contained pipe keeps nonblocking, poll, inheritance and EOF semantics");
}

int StandaloneGuest(const fs::path& dir) {
  if (!Check(Lowbox(), "standalone runs in a real AppContainer")) return 1;
  if (!CheckStandalonePipe()) return 1;
  const DWORD broker_pid = gesher::binder_endpoint::Current().StandaloneRecordBrokerPid();
  if (!Check(broker_pid != 0, "standalone can identify its retained broker process")) return 1;
  std::printf("broker-pid %lu\n", broker_pid);
  std::fflush(stdout);
  const int binder = open("/dev/binder", O_RDWR);
  if (binder >= 0) close(binder);
  if (!Check(binder < 0, "standalone remains outside the Android Binder device")) return 1;
  const int fd = OpenData(dir);
  auto lock = Range(F_WRLCK);
  if (!Check(fd >= 0 && fcntl(fd, F_SETLK, &lock) == 0, "standalone obtains a real record lock")) return 1;
  auto args = L" --observe " + std::to_wstring(GetCurrentProcessId()) + L" ";
  gesher::exec::AppendQuoted(&args, dir.native());
  if (!Check(StandaloneChild(args, GetStdHandle(STD_INPUT_HANDLE)) && close(fd) == 0,
             "authenticated standalone descendant observes its parent's range")) return 1;
  const int alias = OpenData(dir);
  return Check(alias >= 0 && fcntl(alias, F_OFD_SETLK, &lock) == 0 &&
               StandaloneChild(L" --query-inherited-file", reinterpret_cast<HANDLE>(_get_osfhandle(alias))) &&
               close(alias) == 0, "standalone keeps inherited OFD aliases") ? 0 : 1;
}

bool BrokerStopped(const fs::path& dir) {
  std::ifstream log(dir / L"standalone.log");
  std::string line;
  constexpr std::string_view tag = "broker-pid ";
  while (std::getline(log, line)) {
    if (!line.starts_with(tag)) continue;
    const auto pid = std::strtoul(line.c_str() + tag.size(), nullptr, 10);
    if (pid == 0) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    const bool stopped = WaitForSingleObject(process, 1000) == WAIT_OBJECT_0;
    CloseHandle(process);
    return stopped;
  }
  return false;
}

int StandaloneSupervisor(const fs::path& dir) {
  const auto profile = Profile(dir, kStandalone);
  const fs::path exe(launch::ExePath());
  if (!Check(launch::EnsureAppContainer(profile, profile) == ERROR_SUCCESS &&
             launch::GrantToAppContainer(U16(exe.native()), profile, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false) == 0 &&
             launch::GrantToAppContainer(U16((dir / L"shared").native()), profile,
                                         FILE_GENERIC_READ | FILE_GENERIC_WRITE, false) == 0,
             "standalone grants exact executable and fixture file")) return 1;
  uint32_t exit = STILL_ACTIVE;
  const auto result = launch::RunContainedStandalone(Quote(exe) + u" --standalone-guest " + Quote(dir),
      {profile, profile, {}}, {}, 11003, &exit);
  if (result != ERROR_SUCCESS || exit != 0) {
    std::printf("standalone supervisor result %lu child exit %lu\n",
                static_cast<unsigned long>(result), static_cast<unsigned long>(exit));
  }
  return Check(result == ERROR_SUCCESS && exit == 0 && BrokerStopped(dir),
               "standalone lock-only broker completes and shuts down") ? 0 : 1;
}

int ReadPinnedSource(const fs::path& dir) {
  if (!Check(Lowbox(), "enrolled source reader executes in a genuine AppContainer")) return 1;
  const auto source = dir / L"source.bin";
  HANDLE ordinary = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, 0, nullptr);
  if (ordinary != INVALID_HANDLE_VALUE) CloseHandle(ordinary);
  if (!Check(ordinary == INVALID_HANDLE_VALUE, "source ACL independently denies lowbox read")) return 1;
  auto path = source.u8string();
  const auto* name = reinterpret_cast<const char*>(path.c_str());
  const int first = open(name, O_RDONLY | O_CLOEXEC);
  const int second = open(name, O_RDONLY | O_CLOEXEC);
  const int writable = open(name, O_RDWR | O_CLOEXEC);
  char found[2]{};
  const bool okay = first >= 0 && second >= 0 && writable < 0 &&
      read(first, found, sizeof(found)) == sizeof(found) && found[0] == 'a' && found[1] == 'b' &&
      read(second, found, 1) == 1 && found[0] == 'a';
  if (first >= 0) close(first);
  if (second >= 0) close(second);
  if (writable >= 0) close(writable);
  if (!Check(okay, "enrolled source uses real bytes and independent read-only handles")) return 1;
  static constexpr char expected[] = "scoped input bytes";
  std::array<char, sizeof(expected) - 1> input{};
  if (!Check(read(STDIN_FILENO, input.data(), input.size()) == input.size() &&
             std::memcmp(input.data(), expected, input.size()) == 0,
             "scoped app reads the original redirected stdin")) return 1;
  Mark("scoped guest stdout");
  std::fputs("scoped guest stderr\n", stderr);
  std::fflush(stderr);
  return 0;
}

#include "cohost-directory-probe.inc"

constexpr uint32_t kCohostClipboardProbeBytes = 2 * 65536 + 17;

int CohostGuest() {
  namespace permissions = gesher::cohost_permissions;
  const auto& scope = gesher::cohost_bootstrap::Current();
  if (!Check(Lowbox() && scope && !scope.storage_pipe.empty() && !scope.permission_pipe.empty(),
             "actual cohost receives its authenticated storage and permission scope") ||
      !CheckCohostDirectory(scope.credential)) return 1;
  gesher::binder_bootstrap::Startup boot;
  if (!gesher::binder_bootstrap::ReadStartupRecord(&boot)) return 1;
  HANDLE mapping = gesher::binder_bootstrap::DecodeHandle(boot.cohost_section);
  void* mutable_bytes = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 1);
  if (mutable_bytes) UnmapViewOfFile(mutable_bytes);
  if (!Check(!mutable_bytes, "cohost bootstrap section is read-only")) return 1;
  std::vector<int32_t> permissions_result;
  if (!Check(permissions::Exchange(permissions::Operation::kCheck,
                {u"android.permission.INTERNET"}, &permissions_result) == ERROR_SUCCESS &&
             permissions_result == std::vector<int32_t>{0},
             "genuine cohost permission check reaches the pinned host")) return 1;
  if (!Check(permissions::Exchange(permissions::Operation::kRequest,
                {u"android.permission.INTERNET", u"com.gesher.test.UNDECLARED"},
                &permissions_result) == ERROR_SUCCESS &&
             permissions_result == std::vector<int32_t>{0, -1},
             "cohost permission reply retains exact request order")) return 1;
  permissions::ClipboardFrame input, query, output;
  if (!input.Allocate(kCohostClipboardProbeBytes + 2) || !query.Allocate(sizeof(uint32_t) + 4)) return 1;
  input.data()[0] = input.data()[1] = 0xff; // No tag.
  std::fill_n(query.data(), query.size(), 0);
  query.data()[1] = 1; // One UTF-16 unit (Ω).
  query.data()[2] = 3;
  query.data()[3] = 0xa9;
  for (uint32_t i = 0; i < kCohostClipboardProbeBytes; i++) input.data()[i + 2] = static_cast<uint8_t>(i);
  for (int pass = 0; pass < 12; ++pass) {
    if (!Check(permissions::ClipboardExchange(permissions::Operation::kClipboardWrite, input,
                &output) == ERROR_SUCCESS && output.size() == sizeof(uint32_t) &&
                output.data()[0] == 0x5a,
               "contained clipboard request preserves exact bytes across pipe buffers")) return 1;
    if (!Check(permissions::ClipboardExchange(permissions::Operation::kClipboardRead, query,
                &output) == ERROR_SUCCESS && output.size() == kCohostClipboardProbeBytes,
               "trusted clipboard response spans multiple pipe buffers")) return 1;
    if (!Check(std::equal(input.data() + 2, input.data() + input.size(), output.data()),
               "cohost clipboard response bytes match")) return 1;
  }
  if (!Check(permissions::ClipboardExchange(permissions::Operation::kClipboardMetadata, query,
              &output) == ERROR_SUCCESS && output.size() == 4 * sizeof(uint32_t),
             "contained metadata carries only a fixed-size descriptor")) return 1;
  query.data()[7] = 1;
  if (!Check(permissions::ClipboardExchange(permissions::Operation::kClipboardRead, query,
              &output) == ERROR_SUCCESS && output.size() == sizeof(uint32_t),
             "cached clipboard content still requires an authenticated read receipt")) return 1;
  return Check(StandaloneChild(L" --cohost-descendant", GetStdHandle(STD_INPUT_HANDLE)),
             "enrolled cohost child inherits the sealed scope") ? 0 : 1;
}

int CohostDescendant() {
  namespace permissions = gesher::cohost_permissions;
  std::vector<int32_t> result;
  permissions::ClipboardFrame input, output;
  if (!input.Allocate(sizeof(uint32_t) + 2)) return 1;
  std::fill_n(input.data(), input.size(), 0);
  input.data()[0] = input.data()[1] = 0xff;
  return Check(Lowbox() && gesher::cohost_bootstrap::Current() &&
      permissions::Exchange(permissions::Operation::kCheck,
          {u"android.permission.INTERNET"}, &result) != ERROR_SUCCESS &&
      permissions::ClipboardExchange(permissions::Operation::kClipboardRead, input, &output)
          != ERROR_SUCCESS,
      "cohost native descendant carries metadata but no parent permission or clipboard authority") ? 0 : 1;
}

bool CheckCohost(const fs::path& dir, const fs::path& exe) {
  namespace permissions = gesher::cohost_permissions;
  namespace storage = gesher::rooted_storage;
  const auto ce = dir / L"fixture-ce";
  const auto de = dir / L"fixture-de";
  fs::create_directories(ce / L"scan");
  fs::create_directories(de);
  std::ofstream(ce / L"scan/a") << "a";
  std::ofstream(ce / L"scan/b") << "b";
  using gesher::binder_peer::OwnedHandle;
  auto open_root = [](const fs::path& path) {
    return OwnedHandle(CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE |
        FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  };
  const auto ce_root = open_root(ce), de_root = open_root(de);
  if (ce_root.get() == INVALID_HANDLE_VALUE || de_root.get() == INVALID_HANDLE_VALUE) return false;
  storage::Server storage_authority({ce_root.get(), de_root.get()});
  uint64_t pin = 0;
  if (launch::PinStandaloneInput(U16((dir / L"source.bin").native()), &pin) != ERROR_SUCCESS) return false;
  const auto profile = Profile(dir, L"cohost");
  const bool supplied = launch::EnsureAppContainer(profile, profile) == ERROR_SUCCESS &&
      launch::GrantToAppContainer(U16(exe.native()), profile,
                                  FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false) == 0;
  permissions::Server authority([](permissions::Operation /* operation */,
      const std::vector<std::u16string>& names, std::vector<int32_t>* values) {
    for (const auto& name : names) values->push_back(name == u"android.permission.INTERNET" ? 0 : -1);
    return ERROR_SUCCESS;
  }, [](permissions::Operation operation, const permissions::ClipboardFrame& input,
        permissions::ClipboardFrame* output) -> DWORD {
    if (operation == permissions::Operation::kClipboardWrite) {
      if (input.size() != kCohostClipboardProbeBytes + 2 ||
          input.data()[0] != 0xff || input.data()[1] != 0xff) return ERROR_INVALID_DATA;
      for (uint32_t i = 0; i < kCohostClipboardProbeBytes; ++i) {
        if (input.data()[i + 2] != static_cast<uint8_t>(i)) return ERROR_INVALID_DATA;
      }
      if (!output->Allocate(sizeof(uint32_t))) return ERROR_NOT_ENOUGH_MEMORY;
      std::fill_n(output->data(), output->size(), 0x5a);
    } else if (operation == permissions::Operation::kClipboardRead) {
      if (input.size() != sizeof(uint32_t) + 4 || input.data()[0] != 0 ||
          input.data()[1] != 1 || input.data()[2] != 3 || input.data()[3] != 0xa9) {
        return ERROR_INVALID_DATA;
      }
      const uint32_t bytes = input.data()[7] == 1 ? sizeof(uint32_t) : kCohostClipboardProbeBytes;
      if (!output->Allocate(bytes)) return ERROR_NOT_ENOUGH_MEMORY;
      for (uint32_t i = 0; i < output->size(); i++) output->data()[i] = static_cast<uint8_t>(i);
    } else if (operation == permissions::Operation::kClipboardMetadata) {
      if (!output->Allocate(4 * sizeof(uint32_t))) return ERROR_NOT_ENOUGH_MEMORY;
      std::fill_n(output->data(), output->size(), 0);
    } else {
      return ERROR_ACCESS_DENIED;
    }
    return ERROR_SUCCESS;
  });
  launch::CohostSpec services{U16(ce.native()), U16(de.native()),
      U16(storage_authority.name()), U16(authority.name()), [&](void* process, void* job) {
        const auto error = storage_authority.Start(process, job);
        return error == ERROR_SUCCESS ? authority.Start(process, job) : error;
      }, DirectoryName(ce_root.get(), VOLUME_NAME_DOS), DirectoryName(de_root.get(), VOLUME_NAME_DOS),
      DirectoryName(ce_root.get(), VOLUME_NAME_NT), DirectoryName(de_root.get(), VOLUME_NAME_NT)};
  uint32_t exit = 1;
  const auto result = supplied ? launch::RunPinnedCohost(Quote(exe) + u" --cohost-guest",
      {profile, profile, {}}, pin, 11005, services, &exit) : ERROR_ACCESS_DENIED;
  launch::ReleaseStandaloneInput(pin);
  return Check(result == ERROR_SUCCESS && exit == 0, "protected host owns a live cohost permission transport");
}

int ScopedServer(const fs::path& dir) {
  if (!launch::HasProtectedLaunchAuthority()) return 1;
  const fs::path exe(launch::ExePath());
  const auto profile = Profile(dir, L"pinned");
  const auto source = dir / L"source.bin";
  std::ofstream(source, std::ios::binary) << "abc";
  uint64_t pin = 0;
  if (!Check(launch::EnsureAppContainer(profile, profile) == ERROR_SUCCESS &&
             launch::GrantToAppContainer(U16(exe.native()), profile,
                                         FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false) == 0 &&
             launch::PinStandaloneInput(U16(source.native()), &pin) == ERROR_SUCCESS,
             "protected server pins original ungranted source")) return 1;
  std::array<uint8_t, 16> installation{};
  installation[0] = 3;
  const int guest = launch::SpawnProtectedApp(Quote(exe) + u" --read-pinned-source " + Quote(dir), {}, {},
      U16((dir / L"scoped-guest.log").native()), {profile, u"gesher.fixture.record.pinned", {u"gesherBinderClient"}},
      11004, installation, 1, pin, false);
  launch::ReleaseStandaloneInput(pin);
  if (!Check(guest > 0 && WaitExit(guest), "protected server executes the pinned source admission")) return 1;
  if (!CheckCohost(dir, exe)) return 1;
  Mark("scoped server ready");
  Sleep(INFINITE);
  return 1;
}

int ScopedSupervisor(const fs::path& runtime, const fs::path& dir) {
  const auto device = U16(dir.filename().native()) + u"-scoped";
  const auto input_path = dir / L"scoped-input";
  fs::copy_file(runtime / L"binderd.exe", dir / L"binderd.exe");
  std::ofstream(input_path, std::ios::binary) << "scoped input bytes";
  HANDLE input = CreateFileW(input_path.c_str(), GENERIC_READ, kSharing, nullptr,
                             OPEN_EXISTING, 0, nullptr);
  HANDLE error = CreateFileW((dir / L"scoped-supervisor.err").c_str(), GENERIC_WRITE,
                             kSharing, nullptr, CREATE_NEW, 0, nullptr);
  if (input == INVALID_HANDLE_VALUE || error == INVALID_HANDLE_VALUE) return 1;
  const HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
  const HANDLE stderr_handle = GetStdHandle(STD_ERROR_HANDLE);
  if (!SetStdHandle(STD_INPUT_HANDLE, input) || !SetStdHandle(STD_ERROR_HANDLE, error)) return 1;
  const auto children = launch::StartProtectedDevice(device,
      {Quote(runtime / L"binderd.exe"), Quote(runtime / L"servicemanager.exe"),
       Quote(fs::path(launch::ExePath())) + u" --scoped-server " + Quote(dir)},
      {U16((dir / L"scoped-binder.log").native()), U16((dir / L"scoped-manager.log").native()),
       U16((dir / L"scoped-server.log").native())},
      {u"GESHER_BINDER=" + device}, true);
  SetStdHandle(STD_INPUT_HANDLE, stdin_handle);
  SetStdHandle(STD_ERROR_HANDLE, stderr_handle);
  CloseHandle(input);
  CloseHandle(error);
  if (children.size() != 3 || !WaitMarker(children[2], dir / L"scoped-server.log", "scoped server ready")) {
    return 1;
  }
  std::printf("scoped-actors %d %d %d\n", children[0], children[1], children[2]);
  std::fflush(stdout);
  Sleep(INFINITE);
  return 1;
}

bool CheckScopedDevice(const fs::path& runtime, const fs::path& dir, const fs::path& probe) {
  const auto scope_log = dir / L"scoped-supervisor.log";
  const int supervisor = launch::Spawn(Quote(probe) + u" --scoped-supervisor " +
      Quote(runtime) + u" " + Quote(dir), {}, {}, U16(scope_log.native()), false);
  if (supervisor <= 0) return false;
  std::array<DWORD, 3> pids{};
  const auto deadline = GetTickCount64() + 10000;
  do {
    std::ifstream source(scope_log);
    std::string line;
    while (std::getline(source, line)) {
      if (line.starts_with("scoped-actors ")) {
        std::istringstream roles(line.substr(14));
        if (roles >> pids[0] >> pids[1] >> pids[2]) break;
        pids = {};
      }
    }
    if (pids[2] != 0) break;
    pids = {};
    Sleep(20);
  } while (GetTickCount64() < deadline && launch::ExitCode(supervisor) == STILL_ACTIVE);
  std::array<HANDLE, 3> processes{};
  bool ready = pids[2] != 0;
  for (size_t i = 0; ready && i < pids.size(); ++i) {
    processes[i] = OpenProcess(SYNCHRONIZE, FALSE, pids[i]);
    ready = processes[i] != nullptr && WaitForSingleObject(processes[i], 0) == WAIT_TIMEOUT;
  }
  const bool killed = launch::KillChild(supervisor, launch::ChildStartTime(supervisor));
  const bool stopped = ready && killed &&
      WaitForMultipleObjects(static_cast<DWORD>(processes.size()), processes.data(), TRUE, 5000)
          == WAIT_OBJECT_0;
  for (HANDLE process : processes) if (process != nullptr) CloseHandle(process);
  launch::DeleteAppContainer(Profile(dir, L"pinned"));
  launch::DeleteAppContainer(Profile(dir, L"cohost"));
  return Check(stopped, "scoped device job exits after its exact supervisor dies") &&
      Check(Contains(scope_log, "scoped guest stdout") &&
            !Contains(scope_log, "scoped guest stderr") &&
            Contains(dir / L"scoped-supervisor.err", "scoped guest stderr"),
            "scoped output preserves distinct inherited streams");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wstring(argv[1]) == L"--fork-exec") return ForkExecGuest(argv[2]);
  if (argc == 2 && std::wstring(argv[1]) == L"--fork-exec-binder")
    return Check(CheckForkBinder(), "real exec replacement uses the inherited UID through Binder") ? 0 : 1;
  if (argc == 3 && std::wstring(argv[1]) == L"--server") return Server(argv[2]);
  if (argc == 3 && std::wstring(argv[1]) == L"--hold") return Hold(argv[2]);
  if (argc == 3 && std::wstring(argv[1]) == L"--standalone-guest") return StandaloneGuest(argv[2]);
  if (argc == 3 && std::wstring(argv[1]) == L"--standalone-supervisor") return StandaloneSupervisor(argv[2]);
  if (argc == 3 && std::wstring(argv[1]) == L"--scoped-server") return ScopedServer(argv[2]);
  if (argc == 3 && std::wstring(argv[1]) == L"--read-pinned-source") return ReadPinnedSource(argv[2]);
  if (argc == 2 && std::wstring(argv[1]) == L"--cohost-guest") return CohostGuest();
  if (argc == 2 && std::wstring(argv[1]) == L"--cohost-descendant") return CohostDescendant();
  if (argc == 4 && std::wstring(argv[1]) == L"--scoped-supervisor") return ScopedSupervisor(argv[2], argv[3]);
  if (argc == 2 && std::wstring(argv[1]) == L"--query-inherited-file") return QueryInheritedFile();
  if (argc == 2 && std::wstring(argv[1]) == L"--read-inherited-pipe") return ReadInheritedPipe();
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
  const auto standalone = dir / L"standalone";
  fs::create_directory(standalone);
  const auto standalone_probe = standalone / self.filename();
  fs::copy_file(self, standalone_probe);
  fs::copy_file(runtime / L"binderd.exe", standalone / L"binderd.exe");
  const int supervisor = launch::Spawn(Quote(standalone_probe) + u" --standalone-supervisor " + Quote(dir),
      {}, {}, U16((dir / L"standalone.log").native()), false);
  const bool standalone_ok = Check(supervisor > 0 && WaitExit(supervisor), "real standalone locks");
  if (!standalone_ok && supervisor > 0) launch::KillChild(supervisor, launch::ChildStartTime(supervisor));
  launch::DeleteAppContainer(Profile(dir, kStandalone));
  const auto device = U16(id);
  const auto pids = launch::StartProtectedDevice(device,
      {Quote(runtime / L"binderd.exe"), Quote(runtime / L"servicemanager.exe"), Quote(probe) + u" --server " + Quote(dir)},
      {U16((dir / L"binder.log").native()), U16((dir / L"manager.log").native()), U16((dir / L"server.log").native())},
      {u"GESHER_SANDBOX=appcontainer", u"GESHER_BINDER=" + device});
  bool ok = Check(pids.size() == 3 && WaitExit(pids[2]), "protected server drives fixture");
  if (!ok && pids.size() == 3) launch::KillChild(pids[2], launch::ChildStartTime(pids[2]));
  for (const auto* role : {kOwner, kObserver}) launch::DeleteAppContainer(Profile(dir, role));
  std::wprintf(L"Fixture logs: %ls\n", dir.c_str());
  const bool scoped_ok = CheckScopedDevice(runtime, dir, probe);
  return ok && standalone_ok && scoped_ok ? 0 : 1;
}
