// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "protected_runtime_client.h"
#include "protected_runtime_wire.h"
#include "../binder/protected_data_client.h"
#include "../binder/protected_data_pipe.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <string>

namespace gesher::usb_storage {
namespace {
namespace wire = runtime_wire;
struct State {
  std::mutex mutex;
  std::wstring locator;
  DWORD authority = 0;
  DWORD session = 0;
  HANDLE pipe = nullptr;
  bool configured = false;
  bool failed = false;
  bool initialized = false;
  uint32_t uid = 0;
  uint32_t gid = 0;
  bool system_server = false;
  std::map<uint32_t, uint64_t> children;
  ~State() { if (pipe) CloseHandle(pipe); }
};
State& Local() { static State value; return value; }

bool Number(std::string_view text, DWORD* value) {
  if (text.empty() || text.size() > 10) return false;
  uint64_t parsed = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    parsed = parsed * 10 + static_cast<unsigned>(c - '0');
    if (parsed > MAXDWORD) return false;
  }
  *value = static_cast<DWORD>(parsed);
  return parsed != 0;
}

DWORD Transfer(HANDLE pipe, void* value, DWORD bytes, bool writing) {
  auto* data = static_cast<std::byte*>(value);
  while (bytes) {
    DWORD done = 0;
    const BOOL ok = writing ? WriteFile(pipe, data, bytes, &done, nullptr)
                            : ReadFile(pipe, data, bytes, &done, nullptr);
    if (!ok) return GetLastError();
    if (done == 0 || done > bytes) return ERROR_BROKEN_PIPE;
    data += done;
    bytes -= done;
  }
  return ERROR_SUCCESS;
}

std::wstring Quote(std::wstring_view argument) {
  std::wstring output(1, L'"');
  size_t slashes = 0;
  for (wchar_t c : argument) {
    if (c == L'\\') { ++slashes; continue; }
    output.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    output.push_back(c);
  }
  output.append(slashes * 2, L'\\');
  output.push_back(L'"');
  return output;
}

uint32_t Exchange(State& state, const wire::Request& request, const void* payload, wire::Reply* reply) {
  DWORD status = ERROR_SUCCESS;
  if (!state.pipe) {
    const binder::ProtectedDataEndpoint endpoint{state.locator, state.authority, 0, state.session};
    status = binder::OpenProtectedDataPipe(endpoint, &state.pipe);
  }
  if (status == ERROR_SUCCESS) status = Transfer(state.pipe,
      const_cast<wire::Request*>(&request), sizeof(request), true);
  if (status == ERROR_SUCCESS && request.payload_bytes) {
    status = Transfer(state.pipe, const_cast<void*>(payload), request.payload_bytes, true);
  }
  if (status == ERROR_SUCCESS) status = Transfer(state.pipe, reply, sizeof(*reply), false);
  if (status == ERROR_SUCCESS && (reply->magic != wire::kMagic || reply->version != wire::kVersion ||
      reply->reserved || reply->reserved2 || reply->running > 1 || reply->role > 1 || reply->reserved3)) {
    status = ERROR_INVALID_DATA;
  }
  const bool spawn = request.operation == wire::Operation::kSpawn || request.operation == wire::Operation::kSpawnCommand;
  if (status == ERROR_SUCCESS && reply->status == ERROR_SUCCESS &&
      (spawn ? (!reply->pid || !reply->start_time) :
               (reply->pid != request.pid || reply->start_time != request.start_time))) status = ERROR_INVALID_DATA;
  if (status != ERROR_SUCCESS) {
    if (state.pipe) CloseHandle(state.pipe);
    state.pipe = nullptr;
    state.failed = true;
  }
  return status == ERROR_SUCCESS ? reply->status : status;
}
}  // namespace

extern "C" __declspec(dllexport) void gesher_protected_runtime_anchor() {}
extern "C" __declspec(dllexport) bool gesher_protected_runtime_enabled() {
  auto& state = Local();
  std::lock_guard lock(state.mutex);
  return state.configured;
}
extern "C" __declspec(dllexport) bool gesher_protected_runtime_credentials(uint32_t* uid, uint32_t* gid) {
  if (!uid || !gid) return false;
  auto& state = Local();
  std::lock_guard lock(state.mutex);
  if (!state.initialized) return false;
  *uid = state.uid;
  *gid = state.gid;
  return true;
}
extern "C" __declspec(dllexport) bool gesher_protected_runtime_system_server() {
  auto& state = Local();
  std::lock_guard lock(state.mutex);
  return state.initialized && state.system_server;
}

extern "C" __declspec(dllexport) uint32_t gesher_protected_runtime_rpc(
    const runtime_wire::Request* request, const void* payload, runtime_wire::Reply* reply) try {
  if (!request || !reply || request->magic != wire::kMagic || request->version != wire::kVersion ||
      request->payload_bytes > wire::kMaximumPayload || (!payload && request->payload_bytes)) {
    return ERROR_INVALID_PARAMETER;
  }
  auto& state = Local();
  std::lock_guard lock(state.mutex);
  if (!state.configured || state.failed) return ERROR_ACCESS_DENIED;
  if ((!state.initialized) != (request->operation == wire::Operation::kInitialize)) {
    return ERROR_ACCESS_DENIED;
  }
  const bool spawn = request->operation == wire::Operation::kSpawn || request->operation == wire::Operation::kSpawnCommand;
  if (spawn && state.children.size() >= 128) {
    for (auto child = state.children.begin(); child != state.children.end();) {
      wire::Request query{};
      query.operation = wire::Operation::kStatus;
      query.pid = child->first; query.start_time = child->second;
      wire::Reply result{};
      const DWORD status = Exchange(state, query, nullptr, &result);
      if (state.failed) return status;
      if ((status == ERROR_SUCCESS && !result.running) || status == ERROR_NOT_FOUND ||
          status == ERROR_PROCESS_ABORTED) child = state.children.erase(child);
      else if (status != ERROR_SUCCESS) return status;
      else ++child;
    }
    if (state.children.size() >= 128) return ERROR_NOT_ENOUGH_QUOTA;
  }
  const DWORD status = Exchange(state, *request, payload, reply);
  if (status == ERROR_SUCCESS &&
      request->operation == wire::Operation::kInitialize) {
    state.uid = reply->uid;
    state.gid = reply->gid;
    state.system_server = reply->role == 1;
    state.initialized = true;
  }
  if (status == ERROR_SUCCESS && spawn) {
    state.children[reply->pid] = reply->start_time;
  }
  if (request->operation == wire::Operation::kStatus &&
      ((status == ERROR_SUCCESS && !reply->running) || status == ERROR_NOT_FOUND || status == ERROR_PROCESS_ABORTED)) {
    const auto found = state.children.find(request->pid);
    if (found != state.children.end() && found->second == request->start_time) state.children.erase(found);
  }
  return status;
} catch (const std::bad_alloc&) {
  auto& state = Local();
  std::lock_guard lock(state.mutex);
  if (state.pipe) CloseHandle(state.pipe);
  state.pipe = nullptr;
  state.failed = true;
  return ERROR_NOT_ENOUGH_MEMORY;
}

namespace {
uint32_t Call(wire::Request request, const std::vector<std::byte>& payload, wire::Reply* reply) {
  using Rpc = uint32_t (*)(const wire::Request*, const void*, wire::Reply*);
  auto function = reinterpret_cast<Rpc>(GetProcAddress(GetModuleHandleW(nullptr),
                                                       "gesher_protected_runtime_rpc"));
  if (!function) return ERROR_ACCESS_DENIED;
  request.payload_bytes = static_cast<uint32_t>(payload.size());
  return function(&request, payload.data(), reply);
}
}

bool ConfigureProtectedRuntimeStartup(std::string_view locator, std::string_view authority_pid,
                                      std::string_view desktop_session) {
  DWORD authority = 0, session = 0;
  if (!Number(authority_pid, &authority) || !Number(desktop_session, &session)) return false;
  std::wstring wide;
  for (unsigned char c : locator) {
    if (c > 0x7f || c == 0) return false;
    wide.push_back(static_cast<wchar_t>(c));
  }
  if (!binder::data_pipe::ValidLocator(wide)) return false;
  auto& state = Local();
  std::lock_guard lock(state.mutex);
  if (state.configured) return false;
  state.locator = std::move(wide);
  state.authority = authority;
  state.session = session;
  state.configured = true;
  return true;
}

bool ProtectedRuntimeEnabled() {
  using Query = bool (*)();
  auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(nullptr),
                                                     "gesher_protected_runtime_enabled"));
  return query && query();
}

bool ProtectedRuntimeCredentials(uint32_t* uid, uint32_t* gid) {
  using Query = bool (*)(uint32_t*, uint32_t*);
  auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(nullptr),
                                                     "gesher_protected_runtime_credentials"));
  return query && query(uid, gid);
}
bool ProtectedRuntimeIsSystemServer() {
  using Query = bool (*)();
  auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(nullptr),
                                                     "gesher_protected_runtime_system_server"));
  return query && query();
}

uint32_t ProtectedInitializeResources(ProtectedStartupResources* resources) {
  if (!resources) return ERROR_INVALID_PARAMETER;
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return GetLastError();
  wire::Request request{};
  request.operation = wire::Operation::kInitialize;
  request.pid = GetCurrentProcessId();
  request.start_time = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  wire::Reply reply{};
  const DWORD result = Call(request, {}, &reply);
  if (result == ERROR_SUCCESS) {
    for (size_t i = 0; i < 3; ++i) resources->standard_handles[i] = reply.standard_handles[i];
    resources->block_pipe = reply.block_pipe;
    resources->block_gate = reply.block_gate;
    resources->block_revoked = reply.block_revoked;
    resources->signal_section = reply.signal_section;
    resources->signal_event = reply.signal_event;
  }
  return result;
}

uint32_t StartProtectedRuntime(const std::u16string& apk_path) {
  if (apk_path.empty() || apk_path.size() >= 30000 || apk_path.find(u'\0') != std::u16string::npos) {
    return ERROR_INVALID_PARAMETER;
  }
  PWSTR folder = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX64, KF_FLAG_DONT_VERIFY, nullptr, &folder))) {
    return ERROR_PATH_NOT_FOUND;
  }
  std::wstring executable = std::wstring(folder) + L"\\Gesher Raw USB Broker\\gesher-usb-authority.exe";
  CoTaskMemFree(folder);
  const std::wstring path(reinterpret_cast<const wchar_t*>(apk_path.data()), apk_path.size());
  const std::wstring arguments = L"launch-apk " + Quote(path);
  SHELLEXECUTEINFOW launch{};
  launch.cbSize = sizeof(launch);
  launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  launch.lpVerb = L"runas";
  launch.lpFile = executable.c_str();
  launch.lpParameters = arguments.c_str();
  launch.nShow = SW_HIDE;
  if (!ShellExecuteExW(&launch)) return GetLastError();
  if (!launch.hProcess) return ERROR_INVALID_HANDLE;
  // Reader/access selection is an explicit user interaction. Its duration is
  // the user's choice; the elevated helper separately bounds service startup
  // after consent and exits only with the actual launch result.
  const DWORD waited = WaitForSingleObject(launch.hProcess, INFINITE);
  DWORD code = ERROR_TIMEOUT;
  if (waited == WAIT_OBJECT_0 && !GetExitCodeProcess(launch.hProcess, &code)) code = GetLastError();
  if (waited == WAIT_FAILED) code = GetLastError();
  CloseHandle(launch.hProcess);
  return code;
}

uint32_t ProtectedSpawn(const ProtectedSpawnSpec& spec, ProtectedChild* child) try {
  if (!child || child->pid || child->start_time || spec.environment.size() > 32) {
    return ERROR_INVALID_PARAMETER;
  }
  std::vector<std::byte> payload;
  for (const auto* value : {&spec.package_name, &spec.process_name, &spec.apk_path,
                            &spec.apk_sha256, &spec.signer_sha256}) {
    if (!wire::Append(&payload, *value)) return ERROR_INVALID_PARAMETER;
  }
  for (const auto& entry : spec.environment) {
    if (!wire::Append(&payload, entry)) return ERROR_INVALID_PARAMETER;
  }
  wire::Request request{};
  request.operation = wire::Operation::kSpawn;
  request.uid = spec.uid;
  request.target_sdk = spec.target_sdk;
  request.start_sequence = spec.start_sequence;
  wire::Reply reply{};
  const DWORD status = Call(request, payload, &reply);
  if (status == ERROR_SUCCESS) *child = {reply.pid, reply.start_time};
  return status;
} catch (const std::bad_alloc&) { return ERROR_NOT_ENOUGH_MEMORY; }

uint32_t ProtectedChildStatus(uint32_t pid, uint64_t start_time, uint32_t* exit_code, bool* running) {
  if (!pid || !start_time || !exit_code || !running) return ERROR_INVALID_PARAMETER;
  wire::Request request{};
  request.operation = wire::Operation::kStatus;
  request.pid = pid;
  request.start_time = start_time;
  wire::Reply reply{};
  const DWORD status = Call(request, {}, &reply);
  if (status == ERROR_SUCCESS) { *exit_code = reply.exit_code; *running = reply.running != 0; }
  return status;
}

uint32_t ProtectedKill(uint32_t pid, uint64_t start_time) {
  if (!pid || !start_time) return ERROR_INVALID_PARAMETER;
  wire::Request request{};
  request.operation = wire::Operation::kKill;
  request.pid = pid;
  request.start_time = start_time;
  wire::Reply reply{};
  return Call(request, {}, &reply);
}

uint32_t ProtectedSignal(uint32_t pid, uint64_t start_time, int signal) {
  if (!pid || !start_time || signal < 0 || signal > 64) return ERROR_INVALID_PARAMETER;
  wire::Request request{};
  request.operation = wire::Operation::kSignal;
  request.pid = pid;
  request.start_time = start_time;
  request.flags = static_cast<uint32_t>(signal);
  wire::Reply reply{};
  return Call(request, {}, &reply);
}

extern "C" __declspec(dllexport) uint32_t gesher_protected_runtime_signal_child(uint32_t pid, int signal) {
  uint64_t time = 0;
  {
    auto& state = Local();
    std::lock_guard lock(state.mutex);
    const auto found = state.children.find(pid);
    if (!state.initialized || found == state.children.end()) return ERROR_ACCESS_DENIED;
    time = found->second;
  }
  return ProtectedSignal(pid, time, signal);
}

uint32_t ProtectedSignalChild(uint32_t pid, int signal) {
  using Signal = uint32_t (*)(uint32_t, int);
  auto function = reinterpret_cast<Signal>(GetProcAddress(GetModuleHandleW(nullptr),
                                                         "gesher_protected_runtime_signal_child"));
  return function ? function(pid, signal) : ERROR_ACCESS_DENIED;
}

uint32_t ProtectedGrantBlock(uint32_t pid, uint64_t start_time,
    const std::u16string& device_interface, bool allow_write, uint32_t lifetime_milliseconds) {
  if (!pid || !start_time || !lifetime_milliseconds || lifetime_milliseconds > 3600000) {
    return ERROR_INVALID_PARAMETER;
  }
  std::vector<std::byte> payload;
  if (!wire::Append(&payload, device_interface)) return ERROR_INVALID_PARAMETER;
  wire::Request request{};
  request.operation = wire::Operation::kGrantBlock;
  request.pid = pid;
  request.start_time = start_time;
  request.flags = allow_write ? 1 : 0;
  request.lifetime_milliseconds = lifetime_milliseconds;
  wire::Reply reply{};
  return Call(request, payload, &reply);
}

uint32_t ProtectedRevokeBlock(uint32_t pid, uint64_t start_time) {
  if (!pid || !start_time) return ERROR_INVALID_PARAMETER;
  wire::Request request{};
  request.operation = wire::Operation::kRevokeBlock;
  request.pid = pid;
  request.start_time = start_time;
  wire::Reply reply{};
  return Call(request, {}, &reply);
}

uint32_t ProtectedSpawnCommand(const ProtectedCommandSpec& spec, ProtectedCommandChild* child) try {
  if (!child || child->pid || child->start_time || child->wait_handle || spec.arguments.empty() ||
      spec.arguments.size() > 128 || spec.environment.size() > 32) return ERROR_INVALID_PARAMETER;
  std::vector<std::byte> payload;
  if (!wire::Append(&payload, spec.working_directory) ||
      !wire::Append(&payload, std::u16string(1, static_cast<char16_t>(spec.arguments.size())))) {
    return ERROR_INVALID_PARAMETER;
  }
  for (const auto& item : spec.arguments) if (!wire::Append(&payload, item)) return ERROR_INVALID_PARAMETER;
  for (const auto& item : spec.environment) if (!wire::Append(&payload, item)) return ERROR_INVALID_PARAMETER;
  wire::Request request{};
  request.operation = wire::Operation::kSpawnCommand;
  request.flags = spec.privileged_root ? 1 : 0;
  for (size_t i = 0; i < 3; ++i) request.standard_handles[i] = spec.standard_handles[i];
  wire::Reply reply{};
  const DWORD status = Call(request, payload, &reply);
  if (status == ERROR_SUCCESS) *child = {reply.pid, reply.start_time, reply.wait_handle};
  return status;
} catch (const std::bad_alloc&) { return ERROR_NOT_ENOUGH_MEMORY; }
}  // namespace gesher::usb_storage
