// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <windows.h>
#include <sys/statvfs.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include <gesher/binder_bootstrap.h>
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

OwnedHandle Connect(const wchar_t* name) {
  HANDLE pipe = CreateFileW(name, FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
                           0, nullptr, OPEN_EXISTING,
                           SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return {};
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
  return Check(!connection || !Request(connection.get(), request, L"host-path", &reply),
               "Android /data status has no guest-specified path");
}

int Guest(const wchar_t* name, const wchar_t* host_path, const wchar_t* volume_total, bool denied) {
  if (!Check(Lowbox(), "fixture uses an actual Windows AppContainer")) return 1;
  OwnedHandle connection = Connect(name);
  wire::Request request;
  request.operation = wire::Op::kOpen;
  request.flags = wire::kReadWrite | wire::kCreate | wire::kExclusive;
  wire::Reply reply;
  if (denied) {
    return Check(!connection || !Request(connection.get(), request, L"unauthorized", &reply),
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
  return Check(handles_now == handles_before, "abandoned offer is withdrawn by the host") ? 0 : 1;
}

struct Child {
  OwnedHandle process;
  OwnedHandle thread;
};

Child Spawn(const fs::path& executable, const std::wstring& arguments, PSID sid, const fs::path& log) {
  SECURITY_CAPABILITIES capabilities{sid, nullptr, 0, 0};
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  OwnedHandle output_owner(output), input_owner(input);
  if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) return {};
  HANDLE inherited[] = {input, output};
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
  std::vector<BYTE> storage(bytes);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 2, 0, &bytes)) return {};
  const bool configured = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                                     &capabilities, sizeof(capabilities), nullptr, nullptr) &&
      UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                               inherited, sizeof(inherited), nullptr, nullptr);
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input;
  startup.StartupInfo.hStdOutput = output;
  startup.StartupInfo.hStdError = output;
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

bool Host(const fs::path& dir, const fs::path& executable, const std::wstring& profile) {
  for (const auto* area : {L"ce", L"de"}) fs::create_directories(dir / area);
  std::ofstream(dir / L"ce/seed", std::ios::binary) << "original";
  fs::copy_file(executable, dir / executable.filename());
  fs::path fixture = dir / executable.filename();
  ac::Sid sid;
  if (ac::EnsureProfile(profile, profile, &sid) != ERROR_SUCCESS ||
      ac::Grant(fixture.wstring(), sid.value, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false) != ERROR_SUCCESS) {
    return false;
  }
  OwnedHandle root_ce = Pin(dir / L"ce"), root_de = Pin(dir / L"de");
  if (!root_ce || !root_de) return false;
  storage::Server server({root_ce.get(), root_de.get()});
  struct statvfs legacy{};
  if (statvfs("/data", &legacy) != 0) return false;
  const auto arguments = L" \"" + server.name() + L"\" \"" + (dir / L"ce/seed").wstring() + L"\" " +
      std::to_wstring(legacy.f_blocks * legacy.f_frsize);
  Child legitimate = Spawn(fixture, L"--guest" + arguments, sid.value, dir / L"guest.log");
  Child outside = Spawn(fixture, L"--outside" + arguments, sid.value, dir / L"outside.log");
  OwnedHandle job(CreateJobObjectW(nullptr, nullptr));
  const bool ready = legitimate.process && outside.process && job &&
      AssignProcessToJobObject(job.get(), legitimate.process.get()) &&
      server.Start(legitimate.process.get(), job.get()) == ERROR_SUCCESS;
  const bool denied = ready && ResumeThread(outside.thread.get()) != DWORD(-1) && AwaitExit(outside.process.get());
  const bool guest_ok = denied && ResumeThread(legitimate.thread.get()) != DWORD(-1) && AwaitExit(legitimate.process.get());
  if (!guest_ok && legitimate.process) TerminateProcess(legitimate.process.get(), 1);
  if (!denied && outside.process) TerminateProcess(outside.process.get(), 1);
  std::ifstream actual(dir / L"ce/proof", std::ios::binary);
  std::string bytes{std::istreambuf_iterator<char>(actual), std::istreambuf_iterator<char>()};
  return Check(denied && guest_ok && bytes == "owned" && !fs::exists(dir / L"ce/unauthorized") &&
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
