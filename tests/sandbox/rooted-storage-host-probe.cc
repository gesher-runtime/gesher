// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <windows.h>
#include <sqlite3.h>
#include <sys/statvfs.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include <gesher/binder_bootstrap.h>
#include <gesher/cohost_bootstrap.h>
#include <gesher/filesystem_capacity.h>
#include <gesher/rooted_storage_wire.h>

#include "../../compat/binder/peer_registry.h"
#include "../../compat/framework/app_container.h"
#include "../../compat/framework/rooted_storage_server.h"

namespace {
namespace ac = gesher::appcontainer;
namespace fs = std::filesystem;
namespace storage = gesher::rooted_storage;
namespace wire = gesher::rooted_storage_wire;
namespace boot = gesher::binder_bootstrap;
namespace cohost = gesher::cohost_bootstrap;
using gesher::binder_bootstrap::Transfer;
using gesher::binder_peer::OwnedHandle;

bool Check(bool okay, const char* message) {
  std::printf("%s: %s (Win32 %lu)\n", message, okay ? "ok" : "FAILED", GetLastError());
  std::fflush(stdout);
  return okay;
}

bool Lowbox() {
  DWORD value = 0, bytes = 0;
  return GetTokenInformation(GetCurrentProcessToken(), TokenIsAppContainer, &value, sizeof(value), &bytes) && value != 0;
}

OwnedHandle Connect(const wchar_t* name, bool* opened = nullptr) {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  const ULONGLONG deadline = GetTickCount64() + 5000;
  do {
    pipe = CreateFileW(name, FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
                       0, nullptr, OPEN_EXISTING,
                       SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) break;
    const DWORD error = GetLastError();
    if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) return {};
    Sleep(2);
  } while (GetTickCount64() < deadline);
  if (pipe == INVALID_HANDLE_VALUE) return {};
  if (opened != nullptr) *opened = true;
  OwnedHandle connection(pipe);
  DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
  if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) return {};
  return connection;
}

bool Request(HANDLE pipe, wire::Request request, std::wstring_view path, wire::Reply* reply) {
  request.path_units = static_cast<uint32_t>(path.size());
  const ULONGLONG deadline = GetTickCount64() + 3000;
  return Transfer(pipe, &request, sizeof(request), true, deadline) &&
      Transfer(pipe, const_cast<wchar_t*>(path.data()), static_cast<DWORD>(path.size() * sizeof(wchar_t)), true, deadline) &&
      Transfer(pipe, reply, sizeof(*reply), false, deadline) &&
      reply->magic == wire::kMagic && reply->version == wire::kVersion;
}

bool CompleteOpen(HANDLE pipe) {
  wire::OpenAck acknowledgement;
  wire::OpenAck response;
  const ULONGLONG deadline = GetTickCount64() + 3000;
  return Transfer(pipe, &acknowledgement, sizeof(acknowledgement), true, deadline) &&
      Transfer(pipe, &response, sizeof(response), false, deadline) &&
      response.magic == wire::kOpenCommitMagic && response.version == wire::kVersion;
}

bool AwaitExit(HANDLE process) {
  DWORD status = 1;
  const DWORD wait = WaitForSingleObject(process, 8000);
  GetExitCodeProcess(process, &status);
  if (wait == WAIT_OBJECT_0 && status == 0) return true;
  std::printf("contained child wait=%lu exit=%lu\n", wait, status);
  return false;
}

bool CheckCapacity(const wchar_t* name, uint64_t expected_total) {
  OwnedHandle connection = Connect(name);
  wire::Request request;
  request.operation = wire::Op::kStatAndroidData;
  wire::Reply reply;
  wire::Filesystem stats;
  const bool okay = connection && Request(connection.get(), request, L"", &reply) && reply.status == 0 &&
      reply.handle == 0 && reply.flags == 0 &&
      Transfer(connection.get(), &stats, sizeof(stats), false, GetTickCount64() + 3000) &&
      stats.total_bytes == expected_total && expected_total > 0 && stats.available_bytes <= stats.free_bytes;
  if (!Check(okay, "Android /data status preserves the actual host backing capacity without a handle")) return false;
  connection.reset();
  connection = Connect(name);
  return Check(connection && !Request(connection.get(), request, L"host-path", &reply),
               "Android /data status has no guest-specified path");
}

bool SqliteValue(sqlite3* db, const char* sql, std::string_view expected) {
  sqlite3_stmt* stmt = nullptr;
  const int prepared = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
  const auto* value = prepared == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW
      ? sqlite3_column_text(stmt, 0) : nullptr;
  const bool matches = value && reinterpret_cast<const char*>(value) == expected;
  return sqlite3_finalize(stmt) == SQLITE_OK && matches;
}

bool CheckSqlite(const fs::path& seed) {
  const auto& scope = cohost::Current();
  if (!Check(scope && !scope.storage_pipe.empty(), "SQLite guest has the sealed standalone storage scope")) return false;
  const auto name = (seed.parent_path() / L"rooted.db").u8string();
  const auto* path = reinterpret_cast<const char*>(name.c_str());
  sqlite3* writer = nullptr;
  sqlite3* reader = nullptr;
  const int first = sqlite3_open_v2(path, &writer, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  bool okay = Check(first == SQLITE_OK, "SQLite creates a database through the real rooted Win32 VFS");
  if (!okay) {
    std::fprintf(stderr, "rooted SQLite open: %d (extended %d, system %d) %s\n", first,
                 sqlite3_extended_errcode(writer), sqlite3_system_errno(writer), sqlite3_errmsg(writer));
  }
  if (okay) {
    okay = Check(SqliteValue(writer, "PRAGMA journal_mode=WAL", "wal") &&
                 sqlite3_exec(writer, "CREATE TABLE rows(value); INSERT INTO rows VALUES(42);",
                              nullptr, nullptr, nullptr) == SQLITE_OK,
                 "rooted SQLite writes a committed row using WAL and shared memory");
  }
  if (okay) {
    okay = Check(sqlite3_open_v2(path, &reader, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK &&
                 SqliteValue(reader, "SELECT value FROM rows", "42"),
                 "a concurrent read-only SQLite connection sees the actual committed row");
  }
  if (reader && sqlite3_close(reader) != SQLITE_OK) okay = false;
  if (writer && sqlite3_close(writer) != SQLITE_OK) okay = false;
  if (!okay) return false;
  auto* vfs = sqlite3_vfs_find(nullptr);
  char full[32768]{};
  int exists = 0;
  return Check(vfs && vfs->xFullPathname(vfs, path, sizeof(full), full) == SQLITE_OK &&
               vfs->xAccess(vfs, full, SQLITE_ACCESS_EXISTS, &exists) == SQLITE_OK && exists &&
               vfs->xDelete(vfs, full, 0) == SQLITE_OK &&
               vfs->xAccess(vfs, full, SQLITE_ACCESS_EXISTS, &exists) == SQLITE_OK && !exists,
               "SQLite uses brokered metadata and deletes its actual database");
}

int Guest(const wchar_t* name, const wchar_t* host_path, const wchar_t* volume_total, bool denied) {
  if (!Check(Lowbox(), "fixture uses an actual Windows AppContainer")) return 1;
  bool opened = false;
  OwnedHandle connection = Connect(name, &opened);
  wire::Request request;
  request.operation = wire::Op::kOpen;
  request.flags = wire::kReadWrite | wire::kCreate | wire::kExclusive;
  wire::Reply reply;
  if (denied) {
    return Check(opened && (!connection || !Request(connection.get(), request, L"unauthorized", &reply)),
                 "same AppContainer SID outside the retained job is denied") ? 0 : 1;
  }
  HANDLE direct = CreateFileW(host_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (direct != INVALID_HANDLE_VALUE) CloseHandle(direct);
  if (!Check(direct == INVALID_HANDLE_VALUE, "guest cannot directly open selected host root's original file") ||
      !Check(connection && Request(connection.get(), request, L"proof", &reply) && reply.status == 0 &&
             reply.flags == wire::kCreated && reply.handle != 0 && CompleteOpen(connection.get()),
             "live job member receives and acknowledges its exact file handle")) return 1;
  connection.reset();
  OwnedHandle file(gesher::binder_bootstrap::DecodeHandle(reply.handle));
  DWORD count = 0, information = 0;
  if (!Check(GetHandleInformation(file.get(), &information) && !(information & HANDLE_FLAG_INHERIT) &&
             WriteFile(file.get(), "owned", 5, &count, nullptr) && count == 5,
             "committed noninheritable descriptor writes real selected data")) return 1;
  wire::Filesystem file_volume;
  if (!Check(gesher::QueryFilesystemCapacity(file.get(), &file_volume) == 0 && file_volume.total_bytes > 0 &&
             file_volume.available_bytes <= file_volume.free_bytes,
             "AppContainer queries the actual volume using the exact scoped file handle")) return 1;
  file.reset();
  if (!CheckCapacity(name, _wcstoui64(volume_total, nullptr, 10))) return 1;

  request.source_area = wire::kFirstExternalArea;
  connection = Connect(name);
  if (!Check(connection && Request(connection.get(), request, L"proof", &reply) && reply.status == 0 &&
             reply.handle != 0 && CompleteOpen(connection.get()), "admitted external root supplies a real handle")) return 1;
  connection.reset();
  file.reset(gesher::binder_bootstrap::DecodeHandle(reply.handle));
  if (!Check(WriteFile(file.get(), "external", 8, &count, nullptr) && count == 8,
             "external proof writes through the bound scope")) return 1;
  file.reset();
  connection = Connect(name);
  request.source_area = wire::kFirstExternalArea + 1;
  if (!Check(connection && Request(connection.get(), request, L"proof", &reply) &&
             reply.status != 0 && reply.handle == 0, "unadmitted external root cannot supply a handle")) return 1;
  connection.reset();
  request.source_area = wire::kCredential;

  connection = Connect(name);
  if (!Check(connection && Request(connection.get(), request, L"../outside", &reply) &&
             reply.status == EINVAL && reply.handle == 0,
             "scoped root rejects relative parent access")) return 1;
  connection.reset();
  request.flags = wire::kRead;
  DWORD handles_before = 0;
  if (!GetProcessHandleCount(GetCurrentProcess(), &handles_before)) return 1;
  connection = Connect(name);
  if (!Check(connection && Request(connection.get(), request, L"proof", &reply) &&
             reply.status == 0 && reply.handle != 0, "open offer reached peer before acknowledgement")) return 1;
  connection.reset();
  const ULONGLONG deadline = GetTickCount64() + 3000;
  DWORD handles_now = 0;
  do {
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles_now)) return 1;
    if (handles_now <= handles_before) break;
    Sleep(5);
  } while (GetTickCount64() < deadline);
  const fs::path seed(host_path);
  const auto data = seed.parent_path().parent_path().native();
  return Check(handles_now == handles_before, "abandoned offer is withdrawn by the host") &&
      _wputenv_s(L"GESHER_APP_PACKAGE", L"com.gesher.test.rooted") == 0 &&
      _wputenv_s(L"ANDROID_DATA", data.c_str()) == 0 &&
      _wputenv_s(L"GESHER_STORAGE_MEDIA_NT_PATH", L"") == 0 && CheckSqlite(seed) ? 0 : 1;
}

struct Child {
  OwnedHandle process;
  OwnedHandle thread;
};

Child Spawn(const fs::path& executable, const std::wstring& arguments, PSID sid, const fs::path& log,
            boot::Startup* record = nullptr) {
  SECURITY_CAPABILITIES capabilities{sid, nullptr, 0, 0};
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  OwnedHandle output_owner(output), input_owner(input);
  if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) return {};
  std::vector<HANDLE> inherited{input, output};
  if (record) {
    inherited.push_back(boot::DecodeHandle(record->cohost_section));
    inherited.push_back(boot::DecodeHandle(record->cohost_host));
    record->null_device = boot::EncodeHandle(input);
    record->broker = record->cohost_host;
  }
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
  std::vector<BYTE> storage(bytes);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 2, 0, &bytes)) return {};
  const bool configured = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                                     &capabilities, sizeof(capabilities), nullptr, nullptr) &&
      UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                               inherited.data(), inherited.size() * sizeof(HANDLE), nullptr, nullptr);
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input;
  startup.StartupInfo.hStdOutput = output;
  startup.StartupInfo.hStdError = output;
  if (record) {
    startup.StartupInfo.lpReserved2 = reinterpret_cast<BYTE*>(record);
    startup.StartupInfo.cbReserved2 = sizeof(*record);
  }
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION child{};
  std::wstring command = L"\"" + executable.wstring() + L"\" " + arguments;
  const bool started = configured && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
      CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &child);
  DeleteProcThreadAttributeList(attributes);
  if (!started) return {};
  return {OwnedHandle(child.hProcess), OwnedHandle(child.hThread)};
}

OwnedHandle Pin(const fs::path& path) {
  HANDLE result = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  return result == INVALID_HANDLE_VALUE ? OwnedHandle() : OwnedHandle(result);
}

std::wstring ObjectName(HANDLE object, DWORD volume) {
  wchar_t path[32768];
  const DWORD used = GetFinalPathNameByHandleW(object, path, std::size(path), FILE_NAME_NORMALIZED | volume);
  return used && used < std::size(path) ? std::wstring(path, used) : std::wstring{};
}

bool StorageRoot(HANDLE root, const fs::path& path, auto* original, auto* dos, auto* nt) {
  const auto canonical = ObjectName(root, VOLUME_NAME_DOS);
  const auto native = ObjectName(root, VOLUME_NAME_NT);
  return !canonical.empty() && !native.empty() && cohost::Store(original, path.native()) &&
      cohost::Store(dos, canonical) && cohost::Store(nt, native);
}

bool CheckUnisolatedEnrollment(HANDLE contained, PSID sid) {
  namespace peer = gesher::binder_peer;
  namespace identity = gesher::process_identity;
  identity::Process desktop, guest;
  if (!identity::ReadLiveProcess(GetCurrentProcess(), &desktop) ||
      !identity::ReadLiveProcess(contained, &guest)) return false;
  peer::Registry registry(desktop.principal);
  const std::array<uint8_t, 16> installation{1};
  auto copy = [](HANDLE process) {
    HANDLE duplicate = nullptr;
    DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &duplicate, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    return OwnedHandle(duplicate);
  };
  if (!Check(!registry.Enroll(copy(contained), guest.pid, guest.birth,
                 peer::Role::kUnisolatedApplication, 10000, installation, nullptr),
             "a real lowbox token cannot be admitted as unisolated") ||
      !Check(!registry.Enroll(copy(GetCurrentProcess()), desktop.pid, desktop.birth,
                 peer::Role::kApplication, 10000, installation, sid),
             "ordinary token cannot substitute for default isolated launch") ||
      !Check(!registry.Enroll(copy(GetCurrentProcess()), desktop.pid, desktop.birth,
                 peer::Role::kUnisolatedApplication, 10000, {}, nullptr),
             "unisolated admission needs an exact installation binding")) return false;
  if (!Check(registry.Enroll(copy(GetCurrentProcess()), desktop.pid, desktop.birth,
                 peer::Role::kUnisolatedApplication, 10000, installation, nullptr),
             "explicit enrollment accepts the broker's actual desktop identity")) return false;
  const auto lease = registry.FindGeneration(desktop.pid, desktop.birth, peer::Role::kApplication);
  return Check(lease && lease->uid == 10000 && lease->installation == installation,
               "unisolated peer still has the bound Android app role and UID");
}

bool Host(const fs::path& dir, const fs::path& executable, const std::wstring& profile) {
  for (const auto* area : {L"ce", L"de", L"external"}) fs::create_directories(dir / area);
  std::ofstream(dir / L"ce/seed", std::ios::binary) << "original";
  fs::copy_file(executable, dir / executable.filename());
  fs::path fixture = dir / executable.filename();
  ac::Sid sid;
  if (ac::EnsureProfile(profile, profile, &sid) != ERROR_SUCCESS ||
      ac::Grant(fixture.wstring(), sid.value, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false) != ERROR_SUCCESS) {
    return false;
  }
  OwnedHandle root_ce = Pin(dir / L"ce"), root_de = Pin(dir / L"de");
  OwnedHandle root_external = Pin(dir / L"external");
  if (!root_ce || !root_de || !root_external) return false;
  storage::Server server({root_ce.get(), root_de.get(), {root_external.get()}});
  using Metadata = cohost::Metadata;
  OwnedHandle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
      0, sizeof(Metadata), nullptr));
  auto* record = mapping ? static_cast<Metadata*>(MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, sizeof(Metadata))) : nullptr;
  if (!record) return false;
  new (record) Metadata;
  const bool stored = cohost::Store(&record->storage_pipe, server.name()) &&
      StorageRoot(root_ce.get(), dir / L"ce", &record->credential, &record->credential_canonical, &record->credential_nt) &&
      StorageRoot(root_de.get(), dir / L"de", &record->device, &record->device_canonical, &record->device_nt);
  UnmapViewOfFile(record);
  auto inherit = [](HANDLE handle, DWORD access) {
    HANDLE copy = nullptr;
    DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &copy, access, TRUE, 0);
    return OwnedHandle(copy);
  };
  const auto section = inherit(mapping.get(), FILE_MAP_READ);
  const auto host = inherit(GetCurrentProcess(), PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE);
  if (!stored || !section || !host) return false;
  boot::Startup startup;
  startup.endpoint = boot::Endpoint::kStandalone;
  startup.uid = 11005;
  startup.cohost_section = boot::EncodeHandle(section.get());
  startup.cohost_host = boot::EncodeHandle(host.get());
  struct statvfs legacy{};
  if (statvfs("/data", &legacy) != 0) return false;
  const auto arguments = L" \"" + server.name() + L"\" \"" + (dir / L"ce/seed").wstring() + L"\" " +
      std::to_wstring(legacy.f_blocks * legacy.f_frsize);
  Child legitimate = Spawn(fixture, L"--guest" + arguments, sid.value, dir / L"guest.log", &startup);
  Child outside = Spawn(fixture, L"--outside" + arguments, sid.value, dir / L"outside.log");
  OwnedHandle job(CreateJobObjectW(nullptr, nullptr));
  const bool ready = legitimate.process && outside.process && job &&
      CheckUnisolatedEnrollment(legitimate.process.get(), sid.value) &&
      AssignProcessToJobObject(job.get(), legitimate.process.get()) &&
      server.Start(legitimate.process.get(), job.get()) == ERROR_SUCCESS;
  const bool denied = ready && ResumeThread(outside.thread.get()) != DWORD(-1) && AwaitExit(outside.process.get());
  const bool guest_ok = denied && ResumeThread(legitimate.thread.get()) != DWORD(-1) && AwaitExit(legitimate.process.get());
  if (!guest_ok && legitimate.process) TerminateProcess(legitimate.process.get(), 1);
  if (!denied && outside.process) TerminateProcess(outside.process.get(), 1);
  std::ifstream actual(dir / L"ce/proof", std::ios::binary);
  std::string bytes{std::istreambuf_iterator<char>(actual), std::istreambuf_iterator<char>()};
  std::ifstream external(dir / L"external/proof", std::ios::binary);
  std::string external_bytes{std::istreambuf_iterator<char>(external), std::istreambuf_iterator<char>()};
  return Check(denied && guest_ok && bytes == "owned" && external_bytes == "external" &&
               !fs::exists(dir / L"ce/unauthorized") &&
               !fs::exists(dir / L"de/proof"), "production rooted host serves only exact scope and selected area");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 5) return Guest(argv[2], argv[3], argv[4], std::wstring_view(argv[1]) == L"--outside");
  if (argc != 2) return 2;
  std::wstring profile = L"gesher-rooted-fixture-" + std::to_wstring(GetCurrentProcessId());
  wchar_t executable[32768];
  if (GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))) == 0) return 2;
  bool okay = Host(fs::absolute(argv[1]) / profile, executable, profile);
  ac::DeleteContainerProfile(profile);
  return okay ? 0 : 1;
}
