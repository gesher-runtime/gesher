// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "protected_runtime_session.h"
#include "protected_runtime_wire.h"
#include "protected_identity.h"
#include "authority_launch.h"
#include "authority_peer.h"
#include "binder_authority_control.h"
#include "binder_listener_factory.h"
#include "block_session.h"
#include "protected_signal.h"
#include "../framework/app_container.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <sddl.h>
#include <shlobj.h>
#include <winternl.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <mutex>
#include <new>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

namespace gesher::usb_storage {
namespace {
namespace container = appcontainer;
namespace wire = runtime_wire;
struct Close { void operator()(void* value) const { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
struct Local { void operator()(void* value) const { if (value) LocalFree(value); } };
struct ServiceClose { void operator()(SC_HANDLE value) const { if (value) CloseServiceHandle(value); } };
using Handle = std::unique_ptr<void, Close>;
using LocalMemory = std::unique_ptr<void, Local>;
using Service = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ServiceClose>;

std::wstring Wide(const std::u16string& value) {
  return {reinterpret_cast<const wchar_t*>(value.data()), value.size()};
}
bool Same(std::wstring_view first, std::wstring_view second) {
  return first.size() == second.size() && CompareStringOrdinal(first.data(), static_cast<int>(first.size()),
      second.data(), static_cast<int>(second.size()), TRUE) == CSTR_EQUAL;
}
std::wstring Quote(std::wstring_view argument) {
  std::wstring result(1, L'"');
  size_t slashes = 0;
  for (wchar_t c : argument) {
    if (c == L'\\') { ++slashes; continue; }
    result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    result.push_back(c);
  }
  result.append(slashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}
std::wstring GuestPath(std::wstring path) {
  if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') return {};
  const wchar_t drive = path[0] | 0x20;
  std::replace(path.begin(), path.end(), L'\\', L'/');
  // ART and java.io.File use Unix paths. Match Zygote.fromWindowsPath: paths
  // on the installed runtime's drive use /..., other drives use ADR 0045's
  // explicit /mnt/<drive> form. Guest cwd starts on that runtime drive.
  wchar_t executable[32768]{};
  const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
  if (length > 2 && length < std::size(executable) && executable[1] == L':' &&
      (executable[0] | 0x20) == drive) return path.substr(2);
  return L"/mnt/" + std::wstring(1, drive) + path.substr(2);
}
bool LocalPath(std::wstring_view path) {
  if (path.size() < 4 || path[1] != L':' || path[2] != L'\\' ||
      (path[0] | 0x20) < L'a' || (path[0] | 0x20) > L'z' ||
      path.find_first_of(L"\"/:", 2) != std::wstring_view::npos ||
      path.find(L'\0') != std::wstring_view::npos) return false;
  size_t at = 3;
  while (at < path.size()) {
    size_t end = path.find(L'\\', at);
    if (end == std::wstring_view::npos) end = path.size();
    const auto component = path.substr(at, end - at);
    if (component.empty() || component == L"." || component == L".." ||
        component.back() == L'.' || component.back() == L' ') return false;
    at = end + 1;
  }
  return true;
}
std::wstring SidText(PSID sid) {
  wchar_t* raw = nullptr;
  if (!ConvertSidToStringSidW(sid, &raw)) return {};
  const LocalMemory release(raw);
  return raw;
}
uint64_t CreationTime(HANDLE process) {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
  return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
bool HexDigest(std::wstring_view value) {
  return value.size() == 64 && value.find_first_not_of(L"0123456789abcdef") == std::wstring_view::npos;
}
bool PackageName(std::wstring_view value) {
  return !value.empty() && value.size() <= 255 && value.find_first_not_of(
      L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.") == std::wstring_view::npos &&
      value.front() != L'.' && value.back() != L'.' && value.find(L"..") == std::wstring_view::npos;
}

DWORD Hash(HANDLE file, std::wstring* digest) {
  LARGE_INTEGER zero{};
  if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return GetLastError();
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return ERROR_GEN_FAILURE;
  struct CloseAlgorithm { BCRYPT_ALG_HANDLE value; ~CloseAlgorithm() { BCryptCloseAlgorithmProvider(value, 0); } } owner{algorithm};
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) return ERROR_GEN_FAILURE;
  struct CloseHash { BCRYPT_HASH_HANDLE value; ~CloseHash() { BCryptDestroyHash(value); } } hash_owner{hash};
  std::array<BYTE, 65536> buffer{};
  for (;;) {
    DWORD bytes = 0;
    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) return GetLastError();
    if (!bytes) break;
    if (BCryptHashData(hash, buffer.data(), bytes, 0) < 0) return ERROR_GEN_FAILURE;
  }
  BYTE result[32]{};
  if (BCryptFinishHash(hash, result, sizeof(result), 0) < 0) return ERROR_GEN_FAILURE;
  constexpr wchar_t hex[] = L"0123456789abcdef";
  digest->clear();
  for (BYTE value : result) { digest->push_back(hex[value >> 4]); digest->push_back(hex[value & 15]); }
  return SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) ? ERROR_SUCCESS : GetLastError();
}

DWORD PrivateDirectory(const std::wstring& path, PSID runtime_user = nullptr) {
  PSECURITY_DESCRIPTOR raw = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"O:SYG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)S:(ML;OICI;NW;;;LW)",
      SDDL_REVISION_1, &raw, nullptr)) return GetLastError();
  const LocalMemory security(raw);
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), raw, FALSE};
  if (!CreateDirectoryW(path.c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS) return GetLastError();
  const Handle directory(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (directory.get() == INVALID_HANDLE_VALUE) return GetLastError();
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(directory.get(), &info)) return GetLastError();
  if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    return ERROR_ACCESS_DENIED;
  }
  PSID owner = nullptr;
  PACL acl = nullptr;
  PSECURITY_DESCRIPTOR actual = nullptr;
  DWORD status = GetSecurityInfo(directory.get(), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &actual);
  const LocalMemory release(actual);
  if (status != ERROR_SUCCESS) return status;
  if (!owner || !acl || (!IsWellKnownSid(owner, WinLocalSystemSid) &&
      !IsWellKnownSid(owner, WinBuiltinAdministratorsSid))) return ERROR_ACCESS_DENIED;
  for (DWORD i = 0; i < acl->AceCount; ++i) {
    void* entry = nullptr;
    if (!GetAce(acl, i, &entry)) return GetLastError();
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
    if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) return ERROR_ACCESS_DENIED;
    PSID principal = const_cast<DWORD*>(&ace->SidStart);
    if (IsWellKnownSid(principal, WinLocalSystemSid) || IsWellKnownSid(principal, WinBuiltinAdministratorsSid) ||
        (runtime_user && EqualSid(principal, runtime_user))) continue;
    // Previously enrolled exact lowbox packages may own their access. No
    // ordinary desktop user/group ACE is accepted on the protected root.
    constexpr SID_IDENTIFIER_AUTHORITY package = SECURITY_APP_PACKAGE_AUTHORITY;
    if (!IsValidSid(principal) || std::memcmp(GetSidIdentifierAuthority(principal), &package, sizeof(package)) != 0 ||
        *GetSidSubAuthorityCount(principal) < 2 || *GetSidSubAuthority(principal, 0) != SECURITY_APP_PACKAGE_BASE_RID) {
      return ERROR_ACCESS_DENIED;
    }
  }
  return ERROR_SUCCESS;
}

DWORD PipeSecurity(HANDLE process, LocalMemory* descriptor) {
  HANDLE raw = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) return GetLastError();
  const Handle token(raw);
  alignas(TOKEN_GROUPS) BYTE groups[sizeof(TOKEN_GROUPS) + SECURITY_MAX_SID_SIZE]{};
  alignas(TOKEN_APPCONTAINER_INFORMATION) BYTE lowbox[sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE]{};
  DWORD bytes = 0;
  if (!GetTokenInformation(raw, TokenLogonSid, groups, sizeof(groups), &bytes) ||
      !GetTokenInformation(raw, TokenAppContainerSid, lowbox, sizeof(lowbox), &bytes)) return GetLastError();
  const auto* logon = reinterpret_cast<const TOKEN_GROUPS*>(groups);
  const auto* app = reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(lowbox);
  if (logon->GroupCount != 1 || !app->TokenAppContainer) return ERROR_ACCESS_DENIED;
  const std::wstring logon_sid = SidText(logon->Groups[0].Sid), app_sid = SidText(app->TokenAppContainer);
  if (logon_sid.empty() || app_sid.empty()) return ERROR_INVALID_SID;
  const std::wstring policy = L"O:SYG:SYD:P(A;;FA;;;SY)(A;;0x12018b;;;" + logon_sid +
      L")(A;;0x12018b;;;" + app_sid + L")S:(ML;;NW;;;LW)";
  PSECURITY_DESCRIPTOR converted = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(policy.c_str(), SDDL_REVISION_1,
      &converted, nullptr)) return GetLastError();
  descriptor->reset(converted);
  return ERROR_SUCCESS;
}

DWORD WaitIo(HANDLE pipe, OVERLAPPED* pending, HANDLE stop, HANDLE process, DWORD timeout, DWORD* bytes) {
  const HANDLE waits[] = {stop, process, pending->hEvent};
  const DWORD waited = WaitForMultipleObjects(3, waits, FALSE, timeout);
  if (waited == WAIT_OBJECT_0 + 2) return GetOverlappedResult(pipe, pending, bytes, FALSE) ? ERROR_SUCCESS : GetLastError();
  const DWORD error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : waited == WAIT_FAILED ? GetLastError() : ERROR_OPERATION_ABORTED;
  CancelIoEx(pipe, pending);
  GetOverlappedResult(pipe, pending, bytes, TRUE);
  return error;
}
DWORD Transfer(AuthorityPeer& peer, HANDLE stop, void* data, DWORD bytes, bool writing, bool idle = false) {
  const Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event) return GetLastError();
  auto* cursor = static_cast<std::byte*>(data);
  const ULONGLONG deadline = GetTickCount64() + 30000;
  while (bytes) {
    if (!idle && GetTickCount64() >= deadline) return ERROR_TIMEOUT;
    OVERLAPPED pending{};
    pending.hEvent = event.get();
    ResetEvent(event.get());
    DWORD count = 0;
    const BOOL done = writing ? WriteFile(peer.pipe(), cursor, bytes, nullptr, &pending)
                              : ReadFile(peer.pipe(), cursor, bytes, nullptr, &pending);
    if (!done && GetLastError() != ERROR_IO_PENDING) return GetLastError();
    const DWORD status = done ? (GetOverlappedResult(peer.pipe(), &pending, &count, FALSE) ? ERROR_SUCCESS : GetLastError())
        : WaitIo(peer.pipe(), &pending, stop, peer.process(), idle ? INFINITE : 30000, &count);
    if (status != ERROR_SUCCESS) return status;
    if (!count || count > bytes) return ERROR_BROKEN_PIPE;
    cursor += count;
    bytes -= count;
  }
  return ERROR_SUCCESS;
}

struct Profile {
  HANDLE token;
  std::wstring name;
  container::Sid sid;
  DWORD Create() {
    if (!ImpersonateLoggedOnUser(token)) return GetLastError();
    const DWORD status = container::EnsureProfile(name, name, &sid);
    if (!RevertToSelf()) std::abort();
    return status;
  }
};

struct Entry {
  std::unique_ptr<AuthorityLaunch> launch;
  std::unique_ptr<AuthorityPeer> peer;
  std::shared_ptr<Profile> profile;
  std::thread worker;
  uint64_t registration = 0;
  uint64_t parent = 0;
  uint64_t root_app = 0;
  uint64_t start_time = 0;
  uint32_t uid = 0;
  bool system_server = false;
  bool initialized = false;
  bool internet = false;
  bool exit_observed = false;
  bool selected_initial = false;
  std::atomic<bool> finished{false};
  std::wstring package, digest, signer, apk;
  Handle apk_pin;
  std::array<Handle, 3> standard;
  std::unique_ptr<BlockSession> block;
  std::unique_ptr<ProtectedSignalChannel> signals;
  std::vector<std::wstring> app_environment;
  std::wstring working_directory;
};

class Session {
 public:
  Session(std::wstring directory, const ProtectedSessionOptions& options, HANDLE stop)
      : directory_(std::move(directory)), options_(options), stop_(stop) {}
  ~Session() {
    SetEvent(stop_);
    {
      std::lock_guard lock(mutex_);
      for (auto& [id, entry] : processes_) {
        if (entry->launch && WaitForSingleObject(entry->launch->process(), 0) == WAIT_TIMEOUT) {
          TerminateProcess(entry->launch->process(), ERROR_OPERATION_ABORTED);
        }
      }
    }
    if (reaper_worker_.joinable()) reaper_worker_.join();
    for (auto& [id, entry] : processes_) if (entry->worker.joinable()) entry->worker.join();
    processes_.clear();
    if (listener_worker_.joinable()) listener_worker_.join();
    manager_.reset();
    broker_.reset();
  }
  DWORD Run(const std::wstring& executable, std::unique_ptr<HostDisk> selected);
 private:
  DWORD PrepareData(HANDLE desktop, const std::wstring& executable);
  DWORD CreateProfile(const std::wstring& name, std::shared_ptr<Profile>* profile);
  DWORD StartEntry(std::unique_ptr<Entry> entry, const std::wstring& binary, std::wstring arguments,
                   std::vector<std::wstring> additions, Entry** created);
  void Serve(Entry& entry);
  void Reap();
  DWORD TerminateTree(Entry& entry, DWORD code);
  DWORD Dispatch(Entry& entry, const wire::Request& request, const std::vector<std::byte>& payload, wire::Reply* reply);
  DWORD Spawn(Entry& requester, const wire::Request& request, const std::vector<std::byte>& payload, wire::Reply* reply);
  DWORD SpawnCommand(Entry& requester, const wire::Request& request, const std::vector<std::byte>& payload, wire::Reply* reply);
  DWORD Initialize(Entry& entry, wire::Reply* reply);
  DWORD PinApk(const std::wstring& path, const std::wstring& digest, Handle* file);
  DWORD GrantAppFiles(Entry& entry);
  DWORD MakeLogs(Entry& entry);
  std::wstring Environment(uint32_t uid, const std::vector<std::wstring>& additions, const std::wstring& temporary);
  std::wstring VmOptions() const;
  bool AllowedEnvironment(const std::wstring& entry, bool command = false) const;
  Entry* Find(uint32_t pid, uint64_t start_time);
  Entry* Root(Entry& entry);

  std::wstring directory_, runtime_, root_, data_, stage_, base_environment_, binder_data_;
  ProtectedSessionOptions options_;
  HANDLE stop_;
  Handle ready_;
  Handle staged_apk_;
  std::wstring staged_path_;
  std::unique_ptr<ProtectedIdentity> identity_;
  container::Sid runtime_capability_;
  std::shared_ptr<Profile> service_profile_;
  std::unique_ptr<AuthorityLaunch> broker_, manager_;
  std::unique_ptr<BinderAuthorityControl> registration_, listeners_;
  std::unique_ptr<BinderListenerFactory> factory_;
  std::thread listener_worker_;
  std::thread reaper_worker_;
  std::mutex mutex_;
  std::map<uint64_t, std::unique_ptr<Entry>> processes_;
  struct Exit {
    uint64_t parent, start_time;
    DWORD pid, code;
    bool application;
  };
  std::map<uint64_t, Exit> exits_;
  std::unique_ptr<HostDisk> selected_;
  uint64_t next_id_ = 3;
  bool application_started_ = false;
};

DWORD Session::CreateProfile(const std::wstring& name, std::shared_ptr<Profile>* profile) {
  auto value = std::make_shared<Profile>();
  value->token = identity_->token();
  value->name = name;
  const DWORD status = value->Create();
  if (status == ERROR_SUCCESS) *profile = std::move(value);
  return status;
}

std::wstring Session::VmOptions() const {
  std::wstring jars;
  for (const wchar_t* name : {L"core-oj", L"core-libart", L"core-icu4j", L"framework", L"gesher-app"}) {
    if (!jars.empty()) jars.push_back(L':');
    jars += GuestPath(runtime_ + L"\\" + name + L".jar");
  }
  return Quote(L"-Xbootclasspath:" + jars) + L" " + Quote(L"-Ximage:" + GuestPath(runtime_ + L"\\framework\\boot.art"));
}
std::wstring Session::Environment(uint32_t uid, const std::vector<std::wstring>& additions,
                                 const std::wstring& temporary) {
  std::wstring result = base_environment_;
  for (const auto& value : {L"GESHER_UID=" + std::to_wstring(uid),
                           L"GESHER_BINDER_UID=" + std::to_wstring(uid),
                           L"TEMP=" + temporary, L"TMP=" + temporary}) {
    result += value; result.push_back(L'\0');
  }
  for (const auto& value : additions) { result += value; result.push_back(L'\0'); }
  result.push_back(L'\0');
  return result;
}

bool Session::AllowedEnvironment(const std::wstring& value, bool command) const {
  const size_t equal = value.find(L'=');
  if (equal == std::wstring::npos || value.find(L'\0') != std::wstring::npos || value.size() > 8192) return false;
  const auto key = std::wstring_view(value).substr(0, equal);
  for (const wchar_t* allowed : {L"GESHER_DISPLAY", L"GESHER_APP_WINDOW", L"GESHER_SEPARATE_WINDOWS",
      L"GESHER_INET", L"GESHER_PROXY", L"HTTP_PROXY", L"HTTPS_PROXY", L"NO_PROXY", L"GESHER_APP_USER_MODEL_ID"}) {
    if (key == allowed) return true;
  }
  if (command) {
    for (const wchar_t* allowed : {L"LANG", L"LC_ALL", L"LC_CTYPE", L"TZ", L"HOME", L"PWD", L"TMPDIR"}) {
      if (key == allowed) return true;
    }
  }
  return false;
}

DWORD Session::PrepareData(HANDLE desktop, const std::wstring& executable) {
  PWSTR raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DONT_VERIFY, nullptr, &raw))) return ERROR_PATH_NOT_FOUND;
  std::wstring parent(raw);
  CoTaskMemFree(raw);
  if (!LocalPath(parent) || GetDriveTypeW(parent.substr(0, 3).c_str()) != DRIVE_FIXED) return ERROR_ACCESS_DENIED;
  parent += L"\\Gesher Protected Android";
  DWORD status = PrivateDirectory(parent, identity_->sid());
  if (status != ERROR_SUCCESS) return status;
  status = container::Grant(parent, identity_->sid(), FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, false);
  if (status != ERROR_SUCCESS) return status;
  alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
  DWORD bytes = 0;
  if (!GetTokenInformation(desktop, TokenUser, user, sizeof(user), &bytes)) return GetLastError();
  const std::wstring name = SidText(reinterpret_cast<TOKEN_USER*>(user)->User.Sid);
  if (name.empty()) return ERROR_INVALID_SID;
  root_ = parent + L"\\" + name;
  status = PrivateDirectory(root_, identity_->sid());
  if (status != ERROR_SUCCESS) return status;
  status = container::Grant(root_, identity_->sid(), FILE_ALL_ACCESS, true);
  if (status != ERROR_SUCCESS) return status;
  data_ = root_ + L"\\data";
  stage_ = root_ + L"\\stage";
  for (const auto& path : {data_, stage_, root_ + L"\\logs", root_ + L"\\tmp"}) {
    status = PrivateDirectory(path, identity_->sid());
    if (status != ERROR_SUCCESS) return status;
    status = container::Grant(path, identity_->sid(), FILE_ALL_ACCESS, true);
    if (status != ERROR_SUCCESS) return status;
  }
  const std::wstring realm = binder_data_.substr(binder_data_.size() - 32);
  status = CreateProfile(L"Gesher.Protected.Server." + realm, &service_profile_);
  if (status != ERROR_SUCCESS) return status;
  for (const auto& path : {root_, data_, stage_, root_ + L"\\logs", root_ + L"\\tmp"}) {
    // Each trusted root intentionally protects its DACL from inheritance.
    // Give the server its explicit lowbox grant on every such boundary.
    status = container::Grant(path, service_profile_->sid.value, FILE_ALL_ACCESS, true);
    if (status != ERROR_SUCCESS) return status;
  }
  status = container::Grant(parent, service_profile_->sid.value,
      FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, false);
  if (status != ERROR_SUCCESS) return status;
  status = container::Grant(runtime_, identity_->sid(), FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, true);
  if (status == ERROR_SUCCESS) status = container::Grant(runtime_, runtime_capability_.value,
                                                        FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, true);
  if (status != ERROR_SUCCESS) return status;

  // The desktop helper can only grant the distinct runtime principal access to
  // windows. Its complete arguments are chosen here, and its process remains
  // owned by SYSTEM, with legacy extension-point injection disabled.
  const std::wstring sid = SidText(identity_->sid()), capability = SidText(runtime_capability_.value);
  std::wstring command = Quote(executable) + L" prepare-desktop " + Quote(sid) + L" " + Quote(capability);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:SYG:SYD:P(A;;GA;;;SY)",
      SDDL_REVISION_1, &descriptor, nullptr)) return GetLastError();
  const LocalMemory own_descriptor(descriptor);
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<BYTE> storage(size);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) return GetLastError();
  struct DeleteAttributes { LPPROC_THREAD_ATTRIBUTE_LIST value; ~DeleteAttributes() { DeleteProcThreadAttributeList(value); } } release{attributes};
  DWORD64 mitigation = PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON;
  if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
      &mitigation, sizeof(mitigation), nullptr, nullptr)) return GetLastError();
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  wchar_t desktop_name[] = L"winsta0\\default";
  startup.StartupInfo.lpDesktop = desktop_name;
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION process{};
  if (!CreateProcessAsUserW(desktop, executable.c_str(), command.data(), &security, &security,
      FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, directory_.c_str(),
      &startup.StartupInfo, &process)) return GetLastError();
  const Handle helper(process.hProcess), thread(process.hThread);
  const DWORD waited = WaitForSingleObject(helper.get(), 15000);
  if (waited != WAIT_OBJECT_0) { TerminateProcess(helper.get(), ERROR_TIMEOUT); return ERROR_TIMEOUT; }
  DWORD exit_code = 0;
  if (!GetExitCodeProcess(helper.get(), &exit_code)) return GetLastError();
  if (exit_code != ERROR_SUCCESS) return exit_code;

  // Open the user-selected source under that user's real token. The authority
  // cannot be used to copy an APK from a file that desktop user cannot read.
  if (!ImpersonateLoggedOnUser(desktop)) return GetLastError();
  HANDLE source = CreateFileW(options_.source_apk.c_str(), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  const DWORD open_error = source == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
  if (!RevertToSelf()) std::abort();
  const Handle source_owner(source);
  if (open_error != ERROR_SUCCESS) return open_error;
  BY_HANDLE_FILE_INFORMATION source_info{};
  if (!GetFileInformationByHandle(source, &source_info)) return GetLastError();
  if (source_info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_ACCESS_DENIED;
  std::wstring source_digest;
  status = Hash(source, &source_digest);
  if (status != ERROR_SUCCESS || source_digest != options_.apk_sha256) return status ? status : ERROR_FILE_INVALID;
  const std::wstring staged_directory = stage_ + L"\\" + source_digest;
  status = PrivateDirectory(staged_directory, identity_->sid());
  if (status == ERROR_SUCCESS) status = container::Grant(staged_directory, identity_->sid(), FILE_ALL_ACCESS, true);
  if (status == ERROR_SUCCESS) status = container::Grant(staged_directory, service_profile_->sid.value, FILE_ALL_ACCESS, true);
  if (status != ERROR_SUCCESS) return status;
  staged_path_ = staged_directory + L"\\base.apk";
  HANDLE output = CreateFileW(staged_path_.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
      nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS) {
    output = CreateFileW(staged_path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  } else if (output != INVALID_HANDLE_VALUE) {
    Handle writer(output);
    std::array<BYTE, 65536> buffer{};
    for (;;) {
      DWORD count = 0, wrote = 0;
      if (!ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) return GetLastError();
      if (!count) break;
      if (!WriteFile(output, buffer.data(), count, &wrote, nullptr)) return GetLastError();
      if (wrote != count) return ERROR_WRITE_FAULT;
    }
    if (!FlushFileBuffers(output)) return GetLastError();
    writer.reset();
    output = CreateFileW(staged_path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  }
  staged_apk_.reset(output);
  if (output == INVALID_HANDLE_VALUE) return GetLastError();
  std::wstring staged_digest;
  status = Hash(output, &staged_digest);
  return status == ERROR_SUCCESS && staged_digest != source_digest ? ERROR_FILE_INVALID : status;
}

DWORD Session::MakeLogs(Entry& entry) {
  entry.standard[0].reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING, 0, nullptr));
  if (entry.standard[0].get() == INVALID_HANDLE_VALUE) return GetLastError();
  const std::wstring name = entry.system_server ? L"system_server" : entry.package;
  const std::wstring path = root_ + L"\\logs\\" + name + L".log";
  entry.standard[1].reset(CreateFileW(path.c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (entry.standard[1].get() == INVALID_HANDLE_VALUE) return GetLastError();
  HANDLE duplicate = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), entry.standard[1].get(), GetCurrentProcess(), &duplicate,
      0, FALSE, DUPLICATE_SAME_ACCESS)) return GetLastError();
  entry.standard[2].reset(duplicate);
  const std::wstring logcat_path = path + L"cat";
  const Handle logcat(CreateFileW(logcat_path.c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (logcat.get() == INVALID_HANDLE_VALUE) return GetLastError();
  const DWORD granted = container::Grant(logcat_path, entry.profile->sid.value,
      FILE_GENERIC_READ | FILE_GENERIC_WRITE, false);
  if (granted != ERROR_SUCCESS) return granted;
  return ERROR_SUCCESS;
}

DWORD Session::StartEntry(std::unique_ptr<Entry> entry, const std::wstring& binary,
    std::wstring arguments, std::vector<std::wstring> additions, Entry** created) {
  // Admission and reaping share mutex_. Bound live Windows objects independently
  // of how often an app starts short shell commands.
  if (processes_.size() >= 128) return ERROR_NOT_ENOUGH_QUOTA;
  std::wstring locator;
  DWORD status = BinderListenerFactory::CreateLocator(&locator);
  if (status != ERROR_SUCCESS) return status;
  arguments = L"--gesher-protected-services " + binder_data_ + L" " + std::to_wstring(GetCurrentProcessId()) + L" " +
      std::to_wstring(options_.desktop_session) + L" --gesher-protected-runtime " + locator + L" " +
      std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(options_.desktop_session) + L" " + arguments;
  const std::wstring temporary = root_ + L"\\tmp\\" + (entry->system_server ? L"server" : entry->package);
  if (!CreateDirectoryW(temporary.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return GetLastError();
  status = container::Grant(temporary, entry->profile->sid.value, FILE_ALL_ACCESS, true);
  if (status != ERROR_SUCCESS) return status;
  additions.push_back(L"GESHER_LOG_FILE=" + root_ + L"\\logs\\" +
      (entry->system_server ? L"system_server" : entry->package) + L".logcat");
  const std::wstring environment = Environment(entry->uid, additions, temporary);
  container::Sid internet, network;
  std::vector<SID_AND_ATTRIBUTES> caps{{runtime_capability_.value, SE_GROUP_ENABLED}};
  if (entry->internet) {
    status = container::CapabilitySid(L"internetClient", &internet);
    if (status == ERROR_SUCCESS) status = container::CapabilitySid(L"privateNetworkClientServer", &network);
    if (status != ERROR_SUCCESS) return status;
    caps.push_back({internet.value, SE_GROUP_ENABLED});
    caps.push_back({network.value, SE_GROUP_ENABLED});
  }
  const SECURITY_CAPABILITIES capabilities{entry->profile->sid.value, caps.data(), static_cast<DWORD>(caps.size()), 0};
  const std::wstring& working = entry->working_directory.empty() ? runtime_ : entry->working_directory;
  const AuthorityLaunchSpec spec{binary.c_str(), working.c_str(), arguments, environment,
      capabilities, identity_->sid(), identity_->session()};
  status = AuthorityLaunch::Create(identity_->token(), spec, &entry->launch);
  if (status != ERROR_SUCCESS) return status;
  entry->start_time = CreationTime(entry->launch->process());
  if (!entry->start_time) return ERROR_INVALID_DATA;
  status = ProtectedSignalChannel::Create(entry->launch->process(), &entry->signals);
  if (status != ERROR_SUCCESS) return status;
  LocalMemory descriptor;
  status = PipeSecurity(entry->launch->process(), &descriptor);
  if (status != ERROR_SUCCESS) return status;
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor.get(), FALSE};
  status = AuthorityPeer::Create(entry->launch->process(), locator.c_str(), &security, &entry->peer);
  if (status != ERROR_SUCCESS) return status;
  status = factory_->AllowAppContainer(entry->profile->sid.value);
  if (status == ERROR_SUCCESS) status = registration_->Register(entry->registration, entry->uid,
      entry->system_server ? binder::PeerRole::kSystemServer : binder::PeerRole::kClient, *entry->launch);
  if (status != ERROR_SUCCESS) return status;
  Entry* pointer = entry.get();
  processes_.emplace(entry->registration, std::move(entry));
  pointer->worker = std::thread([this, pointer] { Serve(*pointer); });
  status = pointer->launch->Resume();
  if (status != ERROR_SUCCESS) { registration_->Revoke(pointer->registration); return status; }
  *created = pointer;
  return ERROR_SUCCESS;
}

Entry* Session::Find(uint32_t pid, uint64_t start_time) {
  for (auto& [id, entry] : processes_) {
    if (entry->launch->pid() == pid && entry->start_time == start_time) return entry.get();
  }
  return nullptr;
}
Entry* Session::Root(Entry& entry) {
  const auto found = processes_.find(entry.root_app);
  return found == processes_.end() ? nullptr : found->second.get();
}

DWORD Session::PinApk(const std::wstring& path, const std::wstring& digest, Handle* file) {
  if (!LocalPath(path) || !HexDigest(digest)) return ERROR_INVALID_PARAMETER;
  // PackageManager's verified code lives in its protected install tree. App
  // data and temporary files are never eligible, even with a matching digest:
  // they can change between Android signature verification and this request.
  const std::wstring prefix = data_ + L"\\app\\";
  if (!Same(path, staged_path_) && (path.size() <= prefix.size() ||
      !Same(std::wstring_view(path).substr(0, prefix.size()), prefix))) {
    return ERROR_ACCESS_DENIED;
  }
  file->reset(CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (file->get() == INVALID_HANDLE_VALUE) return GetLastError();
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file->get(), &info)) return GetLastError();
  if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) || info.nNumberOfLinks != 1) {
    return ERROR_ACCESS_DENIED;
  }
  std::wstring final(32768, L'\0');
  const DWORD count = GetFinalPathNameByHandleW(file->get(), final.data(), static_cast<DWORD>(final.size()), FILE_NAME_NORMALIZED);
  if (!count || count >= final.size()) return ERROR_INVALID_DATA;
  final.resize(count);
  if (!Same(final, L"\\\\?\\" + path)) return ERROR_ACCESS_DENIED;
  std::wstring actual;
  const DWORD status = Hash(file->get(), &actual);
  return status == ERROR_SUCCESS && actual != digest ? ERROR_FILE_INVALID : status;
}

DWORD Session::GrantAppFiles(Entry& entry) {
  const std::wstring code = entry.apk.substr(0, entry.apk.find_last_of(L'\\'));
  DWORD status = container::Grant(code, entry.profile->sid.value, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, true);
  if (status != ERROR_SUCCESS) return status;
  const std::wstring package = entry.package;
  for (const auto& path : {data_ + L"\\data\\" + package, data_ + L"\\user_de\\0\\" + package,
      data_ + L"\\misc\\profiles\\cur\\0\\" + package}) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    status = container::Grant(path, entry.profile->sid.value, FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE, true);
    if (status != ERROR_SUCCESS) return status;
  }
  const std::wstring properties = data_ + L"\\system\\gesher-properties";
  if (GetFileAttributesW(properties.c_str()) != INVALID_FILE_ATTRIBUTES) {
    status = container::Grant(properties, runtime_capability_.value, FILE_GENERIC_READ, true);
  }
  return status;
}

DWORD Session::Spawn(Entry& requester, const wire::Request& request,
    const std::vector<std::byte>& payload, wire::Reply* reply) {
  if (!requester.system_server || request.uid < 10000 || request.uid > 19999 ||
      !request.start_sequence || !request.target_sdk || request.pid || request.start_time || request.flags ||
      request.lifetime_milliseconds) return ERROR_ACCESS_DENIED;
  size_t applications = 0;
  for (const auto& [id, entry] : processes_) if (entry->registration == entry->root_app) ++applications;
  for (const auto& [id, exited] : exits_) if (exited.application) ++applications;
  // Android must observe prior application exits before more process slots are
  // admitted. Command exits have a separate bounded short-lived history.
  if (applications >= 64) return ERROR_NOT_ENOUGH_QUOTA;
  wire::Reader reader(payload);
  std::u16string package, process, apk, digest, signer;
  if (!reader.Read(&package) || !reader.Read(&process) || !reader.Read(&apk) ||
      !reader.Read(&digest) || !reader.Read(&signer)) return ERROR_INVALID_DATA;
  auto entry = std::make_unique<Entry>();
  entry->package = Wide(package); entry->apk = Wide(apk); entry->digest = Wide(digest); entry->signer = Wide(signer);
  const std::wstring process_name = Wide(process);
  if (!PackageName(entry->package) || !HexDigest(entry->digest) || !HexDigest(entry->signer) ||
      (process_name != entry->package && !process_name.starts_with(entry->package + L":"))) return ERROR_INVALID_PARAMETER;
  std::vector<std::wstring> environment;
  std::map<std::wstring, bool> keys;
  while (!reader.empty()) {
    std::u16string value;
    if (!reader.Read(&value) || environment.size() >= 32) return ERROR_INVALID_DATA;
    const std::wstring setting = Wide(value);
    if (!AllowedEnvironment(setting) || !keys.emplace(setting.substr(0, setting.find(L'=')), true).second) return ERROR_INVALID_PARAMETER;
    environment.push_back(setting);
  }
  DWORD status = PinApk(entry->apk, entry->digest, &entry->apk_pin);
  if (status != ERROR_SUCCESS) return status;
  entry->uid = request.uid;
  entry->internet = std::find(environment.begin(), environment.end(), L"GESHER_INET=0") == environment.end();
  entry->app_environment = environment;
  entry->parent = requester.registration;
  entry->root_app = entry->registration = next_id_++;
  const std::wstring realm = binder_data_.substr(binder_data_.size() - 32);
  status = CreateProfile(L"Gesher.Protected.App." + realm + L"." + std::to_wstring(entry->uid), &entry->profile);
  if (status == ERROR_SUCCESS) status = GrantAppFiles(*entry);
  if (status == ERROR_SUCCESS) status = MakeLogs(*entry);
  if (status != ERROR_SUCCESS) return status;
  std::wstring arguments = VmOptions() + L" -Xusejit:true android.app.GesherAppProcess --package " +
      Quote(entry->package) + L" --start-seq " + std::to_wstring(request.start_sequence) +
      L" --target-sdk " + std::to_wstring(request.target_sdk);
  const bool selected_package = entry->digest == options_.apk_sha256 && Same(entry->apk, staged_path_);
  entry->selected_initial = selected_package && process_name == entry->package;
  Entry* launched = nullptr;
  status = StartEntry(std::move(entry), runtime_ + L"\\dalvikvm.exe", arguments, std::move(environment), &launched);
  if (status != ERROR_SUCCESS) return status;
  // The selected physical object is retained from the elevated chooser. Grant
  // it once, only to the exact verified APK's primary process.
  if (selected_package && selected_ && process_name == launched->package) {
    status = BlockSession::Adopt(std::move(selected_), launched->launch->process(),
        GetTickCount64() + 3600000, &launched->block);
    if (status != ERROR_SUCCESS) {
      registration_->Revoke(launched->registration);
      TerminateProcess(launched->launch->process(), status);
      return status;
    }
  }
  reply->pid = launched->launch->pid();
  reply->start_time = launched->start_time;
  reply->running = 1;
  return ERROR_SUCCESS;
}

DWORD Session::Initialize(Entry& entry, wire::Reply* reply) {
  if (entry.initialized) return ERROR_ALREADY_ASSIGNED;
  HANDLE signal_section = nullptr, signal_event = nullptr;
  DWORD signal_status = entry.signals->Handoff(&signal_section, &signal_event);
  if (signal_status != ERROR_SUCCESS) return signal_status;
  reply->signal_section = reinterpret_cast<uintptr_t>(signal_section);
  reply->signal_event = reinterpret_cast<uintptr_t>(signal_event);
  // No generic numeric HANDLE is accepted on this path. All transferred
  // resources were created or type-checked by this authority for this process.
  for (size_t i = 0; i < 3; ++i) {
    HANDLE handle = nullptr;
    if (!entry.standard[i] || !DuplicateHandle(GetCurrentProcess(), entry.standard[i].get(),
        entry.launch->process(), &handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) return GetLastError();
    reply->standard_handles[i] = reinterpret_cast<uintptr_t>(handle);
  }
  Entry* root = Root(entry);
  if (root && root->block) {
    BlockGuestHandles handles;
    const DWORD status = root->block->AttachChild(entry.launch->process(), &handles);
    if (status != ERROR_SUCCESS) return status;
    reply->block_pipe = reinterpret_cast<uintptr_t>(handles.pipe);
    reply->block_gate = reinterpret_cast<uintptr_t>(handles.gate);
    reply->block_revoked = reinterpret_cast<uintptr_t>(handles.revoked);
  }
  reply->uid = reply->gid = entry.uid;
  reply->role = entry.system_server ? 1 : 0;
  entry.initialized = true;
  for (auto& standard : entry.standard) standard.reset();
  if (entry.root_app == entry.registration) application_started_ = true;
  if (entry.selected_initial && ready_) {
    SetEvent(ready_.get());
  }
  return ERROR_SUCCESS;
}

DWORD Session::Dispatch(Entry& entry, const wire::Request& request,
    const std::vector<std::byte>& payload, wire::Reply* reply) {
  if (WaitForSingleObject(stop_, 0) != WAIT_TIMEOUT) return ERROR_OPERATION_ABORTED;
  if (request.magic != wire::kMagic || request.version != wire::kVersion || request.reserved ||
      request.payload_bytes != payload.size() || request.payload_bytes > wire::kMaximumPayload) return ERROR_INVALID_DATA;
  if (request.operation != wire::Operation::kSpawnCommand &&
      (request.standard_handles[0] || request.standard_handles[1] || request.standard_handles[2])) return ERROR_INVALID_DATA;
  if (!entry.initialized) {
    if (request.operation != wire::Operation::kInitialize || !payload.empty() || request.uid ||
        request.target_sdk || request.flags || request.start_sequence || request.lifetime_milliseconds ||
        request.pid != entry.launch->pid() || request.start_time != entry.start_time) return ERROR_ACCESS_DENIED;
    reply->pid = request.pid; reply->start_time = request.start_time;
    return Initialize(entry, reply);
  }
  if (request.operation == wire::Operation::kSpawn) return Spawn(entry, request, payload, reply);
  if (request.operation == wire::Operation::kSpawnCommand) return SpawnCommand(entry, request, payload, reply);
  if (request.operation == wire::Operation::kInitialize || request.uid || request.target_sdk || request.start_sequence) return ERROR_INVALID_DATA;
  Entry* child = Find(request.pid, request.start_time);
  reply->pid = request.pid; reply->start_time = request.start_time;
  if (!child) {
    for (auto found = exits_.begin(); found != exits_.end(); ++found) {
      const Exit& exited = found->second;
      if (exited.pid != request.pid || exited.start_time != request.start_time) continue;
      if (!entry.system_server && exited.parent != entry.registration) return ERROR_ACCESS_DENIED;
      if (!payload.empty() || request.lifetime_milliseconds) return ERROR_INVALID_DATA;
      if (request.operation == wire::Operation::kStatus && !request.flags) {
        reply->exit_code = exited.code;
        reply->running = 0;
        exits_.erase(found);
        return ERROR_SUCCESS;
      }
      if ((request.operation == wire::Operation::kKill && !request.flags) ||
          (request.operation == wire::Operation::kSignal && request.flags == 9)) return ERROR_SUCCESS;
      return ERROR_PROCESS_ABORTED;
    }
    return ERROR_NOT_FOUND;
  }
  if (!entry.system_server && child->parent != entry.registration) return ERROR_ACCESS_DENIED;
  switch (request.operation) {
    case wire::Operation::kSignal:
      if (!payload.empty() || request.flags > 64 || request.lifetime_milliseconds) return ERROR_INVALID_DATA;
      if (request.flags == 9) return TerminateTree(*child, 137);
      return child->signals->Send(static_cast<int>(request.flags), entry.launch->pid());
    case wire::Operation::kStatus: {
      if (!payload.empty() || request.flags || request.lifetime_milliseconds) return ERROR_INVALID_DATA;
      const DWORD wait = WaitForSingleObject(child->launch->process(), 0);
      if (wait == WAIT_FAILED) return GetLastError();
      reply->running = wait == WAIT_TIMEOUT;
      DWORD code = 0;
      if (!GetExitCodeProcess(child->launch->process(), &code)) return GetLastError();
      reply->exit_code = code;
      if (!reply->running) child->exit_observed = true;
      return ERROR_SUCCESS;
    }
    case wire::Operation::kKill:
      if (!payload.empty() || request.flags || request.lifetime_milliseconds) return ERROR_INVALID_DATA;
      return TerminateTree(*child, 137);
    case wire::Operation::kRevokeBlock:
      if (!entry.system_server || !payload.empty() || request.flags || request.lifetime_milliseconds) return ERROR_ACCESS_DENIED;
      return child->block ? child->block->Revoke() : ERROR_NOT_FOUND;
    case wire::Operation::kGrantBlock: {
      // Ordinary Android UI cannot create a new machine grant. It may only
      // confirm the exact already-authorized selected object and access mode.
      if (!entry.system_server || request.flags > 1 || !request.lifetime_milliseconds ||
          request.lifetime_milliseconds > 3600000 || !child->block) return ERROR_ACCESS_DENIED;
      wire::Reader reader(payload); std::u16string selected;
      if (!reader.Read(&selected) || !reader.empty() || !Same(Wide(selected), options_.selected_interface) ||
          (request.flags && !options_.allow_write)) return ERROR_ACCESS_DENIED;
      return child->block->Validate();
    }
    default: return ERROR_INVALID_FUNCTION;
  }
}

void Session::Serve(Entry& entry) {
  const Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  DWORD status = event ? ERROR_SUCCESS : GetLastError();
  if (status == ERROR_SUCCESS) {
    OVERLAPPED pending{}; pending.hEvent = event.get();
    if (!ConnectNamedPipe(entry.peer->pipe(), &pending)) {
      status = GetLastError();
      if (status == ERROR_IO_PENDING) { DWORD count = 0; status = WaitIo(entry.peer->pipe(), &pending, stop_, entry.launch->process(), 30000, &count); }
      else if (status == ERROR_PIPE_CONNECTED) status = ERROR_SUCCESS;
    }
  }
  while (status == ERROR_SUCCESS) {
    wire::Request request{};
    status = Transfer(*entry.peer, stop_, &request, 1, false, true);
    if (status == ERROR_SUCCESS) status = Transfer(*entry.peer, stop_, reinterpret_cast<std::byte*>(&request) + 1, sizeof(request) - 1, false);
    if (status != ERROR_SUCCESS) break;
    if (request.payload_bytes > wire::kMaximumPayload) { status = ERROR_INVALID_DATA; break; }
    std::vector<std::byte> payload(request.payload_bytes);
    if (!payload.empty()) status = Transfer(*entry.peer, stop_, payload.data(), request.payload_bytes, false);
    if (status == ERROR_SUCCESS) status = entry.peer->Authenticate();
    if (status != ERROR_SUCCESS) break;
    wire::Reply reply{};
    {
      std::lock_guard lock(mutex_);
      reply.status = Dispatch(entry, request, payload, &reply);
    }
    status = Transfer(*entry.peer, stop_, &reply, sizeof(reply), true);
    if (request.operation == wire::Operation::kInitialize && reply.status != ERROR_SUCCESS) break;
  }
  {
    std::lock_guard lock(mutex_);
    if (TerminateTree(entry, status) != ERROR_SUCCESS || entry.system_server) SetEvent(stop_);
    // These are service-side duplicates of the child's pipe writers. Holding
    // them after exit would keep ProcessBuilder readers from ever seeing EOF.
    for (auto& standard : entry.standard) standard.reset();
  }
  entry.finished.store(true);
}

DWORD Session::TerminateTree(Entry& entry, DWORD code) {
  if (entry.block) entry.block->Revoke();
  DWORD result = ERROR_SUCCESS;
  for (auto& [id, child] : processes_) {
    if (child->parent != entry.registration) continue;
    const DWORD status = TerminateTree(*child, code);
    if (status != ERROR_SUCCESS) result = status;
  }
  registration_->Revoke(entry.registration);
  const DWORD state = WaitForSingleObject(entry.launch->process(), 0);
  if (state == WAIT_TIMEOUT) {
    if (!TerminateProcess(entry.launch->process(), code)) return GetLastError();
    const DWORD waited = WaitForSingleObject(entry.launch->process(), 5000);
    if (waited != WAIT_OBJECT_0) return waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
  } else if (state != WAIT_OBJECT_0) {
    return GetLastError();
  }
  return result;
}

void Session::Reap() {
  while (WaitForSingleObject(stop_, 100) == WAIT_TIMEOUT) {
    std::vector<std::unique_ptr<Entry>> completed;
    {
      std::lock_guard lock(mutex_);
      for (auto found = processes_.begin(); found != processes_.end();) {
        Entry& entry = *found->second;
        if (entry.system_server || !entry.finished.load()) { ++found; continue; }
        DWORD code = ERROR_PROCESS_ABORTED;
        GetExitCodeProcess(entry.launch->process(), &code);
        if (!entry.exit_observed) {
          exits_[entry.registration] = {entry.parent, entry.start_time, entry.launch->pid(), code,
                                       entry.root_app == entry.registration};
        }
        completed.push_back(std::move(found->second));
        found = processes_.erase(found);
      }
      size_t commands = 0;
      for (const auto& [id, exited] : exits_) if (!exited.application) ++commands;
      for (auto found = exits_.begin(); commands > 256 && found != exits_.end();) {
        if (!found->second.application) { found = exits_.erase(found); --commands; }
        else ++found;
      }
      if (application_started_) {
        bool live = false;
        for (const auto& [id, entry] : processes_) {
          if (!entry->system_server && WaitForSingleObject(entry->launch->process(), 0) == WAIT_TIMEOUT) {
            live = true;
            break;
          }
        }
        // A completed protected application session releases the optional
        // authority for the next explicit launch. Never stop another live app
        // or a still-running admitted command to make room for it.
        if (!live) SetEvent(stop_);
      }
    }
    // The workers and media brokers can finish while acquiring their own
    // locks. Never join or destroy those owners while holding mutex_.
    for (auto& entry : completed) if (entry->worker.joinable()) entry->worker.join();
  }
}

DWORD Session::Run(const std::wstring& executable, std::unique_ptr<HostDisk> selected) {
  selected_ = std::move(selected);
  runtime_ = directory_ + L"\\runtime";
  HANDLE desktop_raw = nullptr;
  if (!WTSQueryUserToken(options_.desktop_session, &desktop_raw)) return GetLastError();
  const Handle desktop(desktop_raw);
  DWORD status = ProtectedIdentity::Create(executable, options_.desktop_session, &identity_);
  if (status != ERROR_SUCCESS) return status;
  status = container::CapabilitySid(container::kRuntimeFilesCapability, &runtime_capability_);
  if (status != ERROR_SUCCESS) return status;
  std::wstring registration, listeners;
  status = BinderAuthorityControl::CreateLocator(&registration);
  if (status == ERROR_SUCCESS) status = BinderAuthorityControl::CreateLocator(&listeners);
  if (status == ERROR_SUCCESS) status = BinderListenerFactory::CreateLocator(&binder_data_);
  if (status != ERROR_SUCCESS) return status;
  status = PrepareData(desktop.get(), executable);
  if (status != ERROR_SUCCESS) return status;
  if (!options_.ready_event.empty()) {
    ready_.reset(OpenEventW(EVENT_MODIFY_STATE, FALSE, options_.ready_event.c_str()));
    if (!ready_) return GetLastError();
  }
  wchar_t windows[MAX_PATH]{};
  const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
  if (!length || length >= MAX_PATH) return ERROR_PATH_NOT_FOUND;
  const std::vector<std::wstring> base = {
      L"PATH=" + runtime_ + L";" + windows + L"\\System32", L"SystemRoot=" + std::wstring(windows),
      L"WINDIR=" + std::wstring(windows), L"ANDROID_ROOT=" + GuestPath(runtime_ + L"\\system"),
      L"ANDROID_ART_ROOT=" + GuestPath(runtime_ + L"\\art"), L"ANDROID_I18N_ROOT=" + GuestPath(runtime_ + L"\\i18n"),
      L"ANDROID_TZDATA_ROOT=" + GuestPath(runtime_ + L"\\tzdata"), L"ANDROID_DATA=" + GuestPath(data_),
      L"GESHER_LOGS=" + GuestPath(root_ + L"\\logs"), L"GESHER_BINDER=protected-" + binder_data_.substr(binder_data_.size() - 32)};
  for (const auto& value : base) { base_environment_ += value; base_environment_.push_back(L'\0'); }
  const std::wstring environment = Environment(1000, {}, root_ + L"\\tmp\\server");
  SID_AND_ATTRIBUTES caps[] = {{runtime_capability_.value, SE_GROUP_ENABLED}};
  const SECURITY_CAPABILITIES capabilities{service_profile_->sid.value, caps, 1, 0};
  const std::wstring broker_image = runtime_ + L"\\binderd.exe";
  const std::wstring broker_arguments = L"--gesher-protected-binder " + registration + L" " + listeners + L" " + binder_data_;
  const AuthorityLaunchSpec broker_spec{broker_image.c_str(), runtime_.c_str(), broker_arguments,
      environment, capabilities, identity_->sid(), identity_->session()};
  status = AuthorityLaunch::Create(identity_->token(), broker_spec, &broker_);
  if (status != ERROR_SUCCESS) return status;
  PSID packages[] = {service_profile_->sid.value};
  status = BinderListenerFactory::Create(*broker_, binder_data_, packages, &factory_);
  if (status != ERROR_SUCCESS) return status;
  status = BinderAuthorityControl::Connect(*broker_, registration, listeners, stop_, &registration_, &listeners_);
  if (status != ERROR_SUCCESS) return status;
  listener_worker_ = std::thread([this] { listeners_->ServeListeners(*factory_); SetEvent(stop_); });
  const std::wstring manager_image = runtime_ + L"\\servicemanager.exe";
  const std::wstring manager_arguments = L"--gesher-protected-services " + binder_data_ + L" " +
      std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(options_.desktop_session);
  const AuthorityLaunchSpec manager_spec{manager_image.c_str(), runtime_.c_str(), manager_arguments,
      environment, capabilities, identity_->sid(), identity_->session()};
  status = AuthorityLaunch::Create(identity_->token(), manager_spec, &manager_);
  if (status == ERROR_SUCCESS) status = registration_->Register(1, 1000, binder::PeerRole::kContextManager, *manager_);
  if (status == ERROR_SUCCESS) status = manager_->Resume();
  if (status != ERROR_SUCCESS) return status;
  auto system = std::make_unique<Entry>();
  system->registration = 2;
  system->uid = 1000;
  system->system_server = true;
  system->profile = service_profile_;
  status = MakeLogs(*system);
  if (status != ERROR_SUCCESS) return status;
  Entry* server = nullptr;
  const std::wstring arguments = VmOptions() + L" -Xusejit:false com.gesher.server.SystemServer --launch-initial-apk " + Quote(GuestPath(staged_path_));
  {
    std::lock_guard lock(mutex_);
    status = StartEntry(std::move(system), runtime_ + L"\\dalvikvm.exe", arguments, {}, &server);
  }
  if (status != ERROR_SUCCESS) return status;
  reaper_worker_ = std::thread([this] { Reap(); });
  const HANDLE waits[] = {stop_, server->launch->process(), manager_->process(), broker_->process()};
  const DWORD waited = WaitForMultipleObjects(4, waits, FALSE, INFINITE);
  if (waited == WAIT_OBJECT_0) return ERROR_SUCCESS;
  return waited == WAIT_FAILED ? GetLastError() : ERROR_PROCESS_ABORTED;
}

// Child-command handling and elevated selection are below. All command requests
// are evaluated under the same session mutex as application registration.

bool NullObject(HANDLE handle) {
  using Query = NTSTATUS (NTAPI*)(HANDLE, OBJECT_INFORMATION_CLASS, PVOID, ULONG, PULONG);
  auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
  if (!query) return false;
  alignas(UNICODE_STRING) BYTE buffer[1024]{};
  ULONG length = 0;
  if (query(handle, static_cast<OBJECT_INFORMATION_CLASS>(1), buffer, sizeof(buffer), &length) < 0) return false;
  const auto* name = reinterpret_cast<const UNICODE_STRING*>(buffer);
  const auto first = reinterpret_cast<uintptr_t>(name->Buffer);
  const auto begin = reinterpret_cast<uintptr_t>(buffer);
  if (!first || first < begin || first > begin + sizeof(buffer) || name->Length > sizeof(buffer) ||
      name->Length > begin + sizeof(buffer) - first || name->Length % sizeof(wchar_t)) return false;
  return Same(std::wstring_view(name->Buffer, name->Length / sizeof(wchar_t)), L"\\Device\\Null");
}

DWORD CaptureStandard(HANDLE parent, uint64_t value, size_t index, DWORD broker_pid, Handle* output) {
  if (!value || value == UINT64_MAX || value > UINTPTR_MAX) return ERROR_INVALID_HANDLE;
  HANDLE raw = nullptr;
  if (!DuplicateHandle(parent, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)),
      GetCurrentProcess(), &raw, 0, FALSE, DUPLICATE_SAME_ACCESS)) return GetLastError();
  const Handle captured(raw);
  const DWORD type = GetFileType(raw);
  if (type == FILE_TYPE_DISK) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(raw, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_REPARSE_POINT))) return ERROR_ACCESS_DENIED;
  } else if (type == FILE_TYPE_PIPE) {
    ULONG server = 0;
    DWORD mode = 0;
    if (!GetNamedPipeServerProcessId(raw, &server) || !GetNamedPipeInfo(raw, &mode, nullptr, nullptr, nullptr) ||
        server == GetCurrentProcessId() || server == broker_pid || (mode & PIPE_TYPE_MESSAGE)) return ERROR_ACCESS_DENIED;
  } else if (type != FILE_TYPE_CHAR || !NullObject(raw)) {
    return ERROR_ACCESS_DENIED;
  }
  const DWORD rights = SYNCHRONIZE | (index == 0 ? FILE_READ_DATA | FILE_READ_ATTRIBUTES : FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES);
  HANDLE limited = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), raw, GetCurrentProcess(), &limited, rights, FALSE, 0)) return GetLastError();
  output->reset(limited);
  return ERROR_SUCCESS;
}

DWORD Session::SpawnCommand(Entry& requester, const wire::Request& request,
    const std::vector<std::byte>& payload, wire::Reply* reply) {
  if (requester.system_server || !requester.root_app || request.flags > 1 || request.pid || request.uid ||
      request.target_sdk || request.start_time || request.start_sequence || request.lifetime_milliseconds) return ERROR_ACCESS_DENIED;
  Entry* root = Root(requester);
  if (!root || WaitForSingleObject(root->launch->process(), 0) != WAIT_TIMEOUT) return ERROR_PROCESS_ABORTED;
  if ((request.flags || requester.uid == 0) && (!root->block || root->block->Validate() != ERROR_SUCCESS)) return ERROR_ACCESS_DENIED;
  size_t depth = 0;
  for (const Entry* ancestor = &requester; ancestor && ancestor->parent;) {
    if (++depth >= 32) return ERROR_NOT_ENOUGH_QUOTA;
    const auto parent = processes_.find(ancestor->parent);
    ancestor = parent == processes_.end() ? nullptr : parent->second.get();
  }
  wire::Reader reader(payload);
  std::u16string working, count_text;
  if (!reader.Read(&working) || !reader.Read(&count_text) || count_text.size() != 1 ||
      count_text[0] == 0 || count_text[0] > 128) return ERROR_INVALID_DATA;
  std::vector<std::wstring> arguments;
  for (uint32_t i = 0; i < count_text[0]; ++i) {
    std::u16string item;
    if (!reader.Read(&item)) return ERROR_INVALID_DATA;
    arguments.push_back(Wide(item));
  }
  if (arguments.empty() || (request.flags && arguments.front() != L"--su")) return ERROR_ACCESS_DENIED;
  if (arguments.front() != L"--sh" && arguments.front() != L"--su" &&
      arguments.front() != L"--tool" && arguments.front() != L"--elf" && arguments.front() != L"--spawn") {
    return ERROR_ACCESS_DENIED;
  }
  if (arguments.front() == L"--elf" && (request.flags || requester.uid == 0)) {
    if (arguments.size() < 3 || !LocalPath(arguments[1])) return ERROR_ACCESS_DENIED;
    bool trusted = false;
    for (const wchar_t* tool : {L"mkfs.fat", L"fsck.fat", L"mkfs.exfat", L"fsck.exfat", L"mke2fs", L"e2fsck"}) {
      if (Same(arguments[1], runtime_ + L"\\filesystem-tools\\bin\\" + tool)) trusted = true;
    }
    if (!trusted) return ERROR_ACCESS_DENIED;
  }
  std::vector<std::wstring> environment = root->app_environment;
  std::map<std::wstring, size_t> keys;
  for (size_t i = 0; i < environment.size(); ++i) keys[environment[i].substr(0, environment[i].find(L'='))] = i;
  size_t received = 0;
  while (!reader.empty()) {
    std::u16string value;
    if (!reader.Read(&value) || ++received > 32) return ERROR_INVALID_DATA;
    const std::wstring setting = Wide(value);
    if (!AllowedEnvironment(setting, true)) return ERROR_ACCESS_DENIED;
    const std::wstring key = setting.substr(0, setting.find(L'='));
    if (key == L"GESHER_INET" && !root->internet && setting != L"GESHER_INET=0") return ERROR_ACCESS_DENIED;
    const auto found = keys.find(key);
    if (found == keys.end()) { keys.emplace(key, environment.size()); environment.push_back(setting); }
    else environment[found->second] = setting;
  }
  auto child = std::make_unique<Entry>();
  child->registration = next_id_++;
  child->parent = requester.registration;
  child->root_app = requester.root_app;
  child->uid = request.flags || requester.uid == 0 ? 0 : root->uid;
  child->profile = root->profile;
  child->internet = root->internet;
  child->package = root->package;
  child->digest = root->digest;
  child->signer = root->signer;
  child->apk = root->apk;
  child->working_directory = Wide(working);
  if (!child->working_directory.empty()) {
    if (!LocalPath(child->working_directory)) return ERROR_INVALID_PARAMETER;
    const std::array<std::wstring, 4> permitted = {runtime_, data_ + L"\\data\\" + root->package,
        data_ + L"\\user_de\\0\\" + root->package, root_ + L"\\tmp\\" + root->package};
    bool allowed = false;
    for (const auto& path : permitted) {
      if (Same(child->working_directory, path) || (child->working_directory.size() > path.size() &&
          child->working_directory[path.size()] == L'\\' && Same(std::wstring_view(child->working_directory).substr(0, path.size()), path))) allowed = true;
    }
    if (!allowed) return ERROR_ACCESS_DENIED;
  }
  DWORD status = ERROR_SUCCESS;
  for (size_t i = 0; i < 3 && status == ERROR_SUCCESS; ++i) {
    status = CaptureStandard(requester.launch->process(), request.standard_handles[i], i, broker_->pid(), &child->standard[i]);
  }
  if (status != ERROR_SUCCESS) return status;
  std::wstring command;
  for (const auto& argument : arguments) { if (!command.empty()) command.push_back(L' '); command += Quote(argument); }
  Entry* launched = nullptr;
  status = StartEntry(std::move(child), runtime_ + L"\\gesher-exec.exe", command, std::move(environment), &launched);
  if (status != ERROR_SUCCESS) return status;
  HANDLE wait = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), launched->launch->process(), requester.launch->process(),
      &wait, PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0)) {
    status = GetLastError();
    registration_->Revoke(launched->registration);
    TerminateProcess(launched->launch->process(), status);
    return status;
  }
  reply->pid = launched->launch->pid();
  reply->start_time = launched->start_time;
  reply->running = 1;
  reply->wait_handle = reinterpret_cast<uintptr_t>(wait);
  return ERROR_SUCCESS;
}

DWORD ImportSelection(const std::wstring& executable, const ProtectedSessionOptions& options,
                      std::unique_ptr<HostDisk>* selected) {
  if (!options.chooser_pid || !options.chooser_start_time ||
      options.selected_handle > UINTPTR_MAX || !options.desktop_session ||
      options.selected_volume_handles.size() > 128 ||
      options.selected_volume_handles.size() != options.selected_identity.volume_count) return ERROR_INVALID_PARAMETER;
  const Handle chooser(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE | SYNCHRONIZE,
                                    FALSE, options.chooser_pid));
  if (!chooser) return GetLastError();
  if (CreationTime(chooser.get()) != options.chooser_start_time ||
      WaitForSingleObject(chooser.get(), 0) != WAIT_TIMEOUT) return ERROR_PROCESS_ABORTED;
  std::wstring image(32768, L'\0');
  DWORD size = static_cast<DWORD>(image.size());
  if (!QueryFullProcessImageNameW(chooser.get(), 0, image.data(), &size)) return GetLastError();
  image.resize(size);
  if (!Same(image, executable)) return ERROR_ACCESS_DENIED;
  HANDLE raw = nullptr;
  if (!OpenProcessToken(chooser.get(), TOKEN_QUERY, &raw)) return GetLastError();
  const Handle token(raw);
  TOKEN_ELEVATION elevation{};
  DWORD session = 0;
  alignas(TOKEN_MANDATORY_LABEL) BYTE integrity[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE]{};
  if (!GetTokenInformation(raw, TokenElevation, &elevation, sizeof(elevation), &size) ||
      !GetTokenInformation(raw, TokenSessionId, &session, sizeof(session), &size) ||
      !GetTokenInformation(raw, TokenIntegrityLevel, integrity, sizeof(integrity), &size)) return GetLastError();
  const PSID label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(integrity)->Label.Sid;
  if (!elevation.TokenIsElevated || session != options.desktop_session || !IsValidSid(label) ||
      !IsWellKnownSid(label, WinHighLabelSid)) return ERROR_ACCESS_DENIED;
  if (!options.selected_handle) {
    const auto& identity = options.selected_identity;
    return !options.allow_write && options.selected_interface.empty() && !identity.bytes &&
        !identity.sector_bytes && !identity.media_generation && !identity.flags && !identity.volume_count
        ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
  }
  std::vector<Handle> owned_volumes;
  std::vector<HANDLE> volumes;
  owned_volumes.reserve(options.selected_volume_handles.size());
  volumes.reserve(options.selected_volume_handles.size());
  for (const uint64_t source : options.selected_volume_handles) {
    if (!source || source > UINTPTR_MAX) return ERROR_INVALID_HANDLE;
    HANDLE volume = nullptr;
    if (!DuplicateHandle(chooser.get(), reinterpret_cast<HANDLE>(static_cast<uintptr_t>(source)),
        GetCurrentProcess(), &volume, 0, FALSE, DUPLICATE_SAME_ACCESS)) return GetLastError();
    owned_volumes.emplace_back(volume);
  }
  HANDLE disk = nullptr;
  if (!DuplicateHandle(chooser.get(), reinterpret_cast<HANDLE>(static_cast<uintptr_t>(options.selected_handle)),
      GetCurrentProcess(), &disk, 0, FALSE, DUPLICATE_SAME_ACCESS)) return GetLastError();
  for (auto& volume : owned_volumes) volumes.push_back(volume.release());
  return HostDisk::Adopt(disk, options.allow_write ? Access::kOfflineReadWrite : Access::kReadOnly,
                         options.selected_identity, selected, std::move(volumes));
}

std::wstring Utf8(std::string_view text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return L"(unavailable)";
  std::wstring result(size, L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size)) return L"(unavailable)";
  return result;
}

DWORD SelectDisk(std::wstring_view application, std::wstring* selected_path, bool* allow_write,
                 std::unique_ptr<HostDisk>* selected) {
  std::vector<std::wstring> paths;
  DWORD status = ListDisks(&paths);
  if (status != ERROR_SUCCESS) return status;
  if (paths.size() > 64) return ERROR_BUFFER_OVERFLOW;
  std::vector<std::unique_ptr<HostDisk>> disks;
  std::vector<std::wstring> available, labels;
  for (const auto& path : paths) {
    std::unique_ptr<HostDisk> disk;
    if (HostDisk::Open(path, Access::kReadOnly, &disk) != ERROR_SUCCESS) continue;
    const DiskInfo& info = disk->info();
    labels.push_back(Utf8(info.model) + L"\nSerial: " + (info.serial.empty() ? L"unavailable" : Utf8(info.serial)) +
                     L"; " + std::to_wstring(info.bytes / (1024 * 1024)) + L" MiB");
    available.push_back(path);
    disks.push_back(std::move(disk));
  }
  std::vector<TASKDIALOG_BUTTON> buttons;
  for (size_t i = 0; i < labels.size(); ++i) buttons.push_back({1000 + static_cast<int>(i), labels[i].c_str()});
  buttons.push_back({900, L"Start without card access\nThe application can run; raw card operations remain denied."});
  TASKDIALOGCONFIG dialog{};
  dialog.cbSize = sizeof(dialog);
  dialog.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
  dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  dialog.pszWindowTitle = L"Gesher USB card access";
  dialog.pszMainInstruction = L"Select the real USB reader and card for this application";
  const std::wstring description = L"Application: " + std::wstring(application) +
      L"\n\nOnly the selected card will be available to this application. Disconnecting it ends this grant.";
  dialog.pszContent = description.c_str();
  dialog.cButtons = static_cast<UINT>(buttons.size());
  dialog.pButtons = buttons.data();
  dialog.nDefaultButton = IDCANCEL;
  int choice = IDCANCEL;
  HRESULT shown = TaskDialogIndirect(&dialog, &choice, nullptr, nullptr);
  if (FAILED(shown)) return static_cast<DWORD>(shown);
  if (choice == 900) { selected_path->clear(); *allow_write = false; selected->reset(); return ERROR_SUCCESS; }
  if (choice < 1000 || static_cast<size_t>(choice - 1000) >= disks.size()) return ERROR_CANCELLED;
  const size_t index = static_cast<size_t>(choice - 1000);
  const TASKDIALOG_BUTTON access[] = {
      {100, L"Read only\nAllow this application to read the selected card."},
      {101, L"Read and write\nAllow this application to modify files and partitions on the selected card."}};
  dialog.pszMainInstruction = L"Choose the access you grant to the selected card";
  const std::wstring confirmation = labels[index] + L"\n\nApplication: " + std::wstring(application);
  dialog.pszContent = confirmation.c_str();
  dialog.cButtons = 2; dialog.pButtons = access; dialog.nDefaultButton = 100;
  choice = IDCANCEL;
  shown = TaskDialogIndirect(&dialog, &choice, nullptr, nullptr);
  if (FAILED(shown)) return static_cast<DWORD>(shown);
  if (choice != 100 && choice != 101) return ERROR_CANCELLED;
  status = disks[index]->Validate();
  if (status != ERROR_SUCCESS) return status;
  if (choice == 101) {
    std::unique_ptr<HostDisk> writable;
    status = HostDisk::Open(available[index], Access::kOfflineReadWrite, &writable);
    if (status != ERROR_SUCCESS) return status;
    status = disks[index]->Validate();
    if (status != ERROR_SUCCESS) return status;
    disks[index] = std::move(writable);
  }
  *selected_path = available[index];
  *allow_write = choice == 101;
  *selected = std::move(disks[index]);
  return ERROR_SUCCESS;
}

}  // namespace

DWORD RunProtectedAndroid(const std::wstring& directory, const std::wstring& executable,
    const ProtectedSessionOptions& options, HANDLE stop) try {
  if (!stop || !LocalPath(options.source_apk) || !HexDigest(options.apk_sha256)) return ERROR_INVALID_PARAMETER;
  std::unique_ptr<HostDisk> selected;
  DWORD status = ImportSelection(executable, options, &selected);
  if (status != ERROR_SUCCESS) return status;
  Session session(directory, options, stop);
  return session.Run(executable, std::move(selected));
} catch (const std::bad_alloc&) { return ERROR_NOT_ENOUGH_MEMORY; }
catch (const std::system_error&) { return ERROR_NOT_READY; }

DWORD LaunchProtectedApk(const std::wstring& executable, const wchar_t* apk_path, bool select_storage) try {
  if (!apk_path || !LocalPath(apk_path)) return ERROR_INVALID_PARAMETER;
  // Check before selecting, locking or dismounting media. An existing realm
  // keeps its processes and grants; a second launch cannot replace it.
  const Service state_manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!state_manager) return GetLastError();
  const Service state_service(OpenServiceW(state_manager.get(), L"GesherUsbAuthority", SERVICE_QUERY_STATUS));
  if (!state_service) return GetLastError();
  SERVICE_STATUS_PROCESS initial{};
  DWORD initial_bytes = 0;
  if (!QueryServiceStatusEx(state_service.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&initial),
      sizeof(initial), &initial_bytes)) return GetLastError();
  if (initial.dwCurrentState != SERVICE_STOPPED) return ERROR_BUSY;
  const Handle source(CreateFileW(apk_path, GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (source.get() == INVALID_HANDLE_VALUE) return GetLastError();
  std::wstring digest;
  DWORD status = Hash(source.get(), &digest);
  if (status != ERROR_SUCCESS) return status;
  std::wstring selected_path;
  bool allow_write = false;
  std::unique_ptr<HostDisk> disk;
  status = select_storage ? SelectDisk(apk_path, &selected_path, &allow_write, &disk) : ERROR_SUCCESS;
  if (status != ERROR_SUCCESS) return status;
  HANDLE selected_raw = nullptr;
  HostDiskIdentity identity{};
  std::vector<HANDLE> volume_handles;
  status = disk ? disk->DuplicateForAuthority(&selected_raw, &identity, &volume_handles) : ERROR_SUCCESS;
  if (status != ERROR_SUCCESS) return status;
  const Handle selected(selected_raw);
  struct CloseVolumes {
    std::vector<HANDLE>& handles;
    ~CloseVolumes() { for (HANDLE handle : handles) CloseHandle(handle); }
  } close_volumes{volume_handles};
  DWORD desktop_session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &desktop_session) || !desktop_session) return ERROR_ACCESS_DENIED;
  const uint64_t created = CreationTime(GetCurrentProcess());
  if (!created) return ERROR_INVALID_DATA;
  std::wstring nonce;
  status = BinderAuthorityControl::CreateLocator(&nonce);
  if (status != ERROR_SUCCESS) return status;
  const std::wstring event_name = L"Global\\GesherProtectedReady." + nonce.substr(nonce.size() - 32);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)",
      SDDL_REVISION_1, &descriptor, nullptr)) return GetLastError();
  const LocalMemory own_descriptor(descriptor);
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  const Handle ready(CreateEventW(&security, TRUE, FALSE, event_name.c_str()));
  if (!ready || GetLastError() == ERROR_ALREADY_EXISTS) return ERROR_ALREADY_EXISTS;
  const Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!manager) return GetLastError();
  const Service service(OpenServiceW(manager.get(), L"GesherUsbAuthority",
      SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | SERVICE_STOP));
  if (!service) return GetLastError();
  // The elevated entry point already validated its own fixed installed image.
  // Verify SCM still launches that exact authority, not another registration.
  DWORD bytes = 0;
  QueryServiceConfigW(service.get(), nullptr, 0, &bytes);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !bytes || bytes > 16384) return ERROR_INVALID_DATA;
  std::vector<BYTE> config_bytes(bytes);
  auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(config_bytes.data());
  if (!QueryServiceConfigW(service.get(), config, bytes, &bytes)) return GetLastError();
  if (!Same(config->lpBinaryPathName, Quote(executable) + L" service") ||
      !Same(config->lpServiceStartName, L"LocalSystem") || config->dwServiceType != SERVICE_WIN32_OWN_PROCESS) return ERROR_ACCESS_DENIED;
  std::vector<std::wstring> arguments = {L"android-session", std::to_wstring(desktop_session), apk_path,
      digest, selected_path, std::to_wstring(GetCurrentProcessId()), std::to_wstring(created),
      std::to_wstring(reinterpret_cast<uintptr_t>(selected.get())), std::to_wstring(identity.bytes),
      std::to_wstring(identity.sector_bytes), std::to_wstring(identity.media_generation),
      std::to_wstring(identity.flags), event_name, std::to_wstring(identity.volume_count)};
  for (HANDLE volume : volume_handles) arguments.push_back(std::to_wstring(reinterpret_cast<uintptr_t>(volume)));
  std::vector<const wchar_t*> argv;
  for (const auto& value : arguments) argv.push_back(value.c_str());
  if (!StartServiceW(service.get(), static_cast<DWORD>(argv.size()), argv.data())) {
    const DWORD error = GetLastError();
    return error == ERROR_SERVICE_ALREADY_RUNNING ? ERROR_BUSY : error;
  }
  const ULONGLONG deadline = GetTickCount64() + 90000;
  while (GetTickCount64() < deadline) {
    if (WaitForSingleObject(ready.get(), 100) == WAIT_OBJECT_0) {
      if (disk) disk->ReleaseAfterAuthorityHandoff();
      return ERROR_SUCCESS;
    }
    SERVICE_STATUS_PROCESS state{};
    if (!QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&state),
        sizeof(state), &bytes)) return GetLastError();
    if (state.dwCurrentState == SERVICE_STOPPED) return state.dwWin32ExitCode ? state.dwWin32ExitCode : ERROR_PROCESS_ABORTED;
    status = disk ? disk->Validate() : ERROR_SUCCESS;
    if (status != ERROR_SUCCESS) break;
  }
  SERVICE_STATUS stopped{};
  ControlService(service.get(), SERVICE_CONTROL_STOP, &stopped);
  return status != ERROR_SUCCESS ? status : ERROR_TIMEOUT;
} catch (const std::bad_alloc&) { return ERROR_NOT_ENOUGH_MEMORY; }
}  // namespace gesher::usb_storage
