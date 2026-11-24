// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Elevated, disposable Windows contract for the actual private identity and
// lowbox launcher. Internal service/guest modes exist only in this test image.
#include "../../compat/usb_storage/protected_identity.h"
#include "../../compat/usb_storage/authority_launch.h"
#include "../../compat/framework/app_container.h"
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace {
namespace storage = gesher::usb_storage;
namespace container = gesher::appcontainer;
struct Close { void operator()(void* h) const { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
struct ScClose { void operator()(SC_HANDLE h) const { if (h) CloseServiceHandle(h); } };
struct Free { void operator()(void* p) const { if (p) LocalFree(p); } };
using Handle = std::unique_ptr<void, Close>;
using Service = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ScClose>;
using Local = std::unique_ptr<void, Free>;
struct Failure { DWORD status; const char* stage; };
void Require(bool ok, const char* stage, DWORD status = ERROR_SUCCESS) {
  if (!ok) {
    const DWORD error = status ? status : GetLastError();
    throw Failure{error ? error : ERROR_GEN_FAILURE, stage};
  }
}
void Check(DWORD status, const char* stage) { Require(status == ERROR_SUCCESS, stage, status); }
struct Evidence {
  DWORD magic = 0x47504c54;
  DWORD status = ERROR_SUCCESS;
  DWORD cleanup = ERROR_SUCCESS;
  char stage[96] = "complete";
};
struct Configuration { std::wstring name, image, directory; DWORD session = 0; } config;
SERVICE_STATUS_HANDLE service_status = nullptr;
Handle service_stop;
Handle service_job;

std::wstring Image() {
  std::array<wchar_t, 32768> path{};
  const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  Require(size && size < path.size(), "test-image-path");
  return {path.data(), size};
}
std::wstring Quote(const std::wstring& value) { return L"\"" + value + L"\""; }
Local Security(bool low = false) {
  PSECURITY_DESCRIPTOR raw = nullptr;
  const wchar_t* policy = low ? L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)S:(ML;OICI;NW;;;LW)" :
      L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
  Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      policy, SDDL_REVISION_1, &raw, nullptr), "fixture-security");
  return Local(raw);
}
void PrivateDirectory(const std::wstring& path, bool low = false) {
  const Local descriptor = Security(low);
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor.get(), FALSE};
  Require(CreateDirectoryW(path.c_str(), &security), "fixture-directory-create");
}
void WriteNew(const std::wstring& path, const void* data, DWORD bytes) {
  const Local descriptor = Security();
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor.get(), FALSE};
  const Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
      CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  Require(file.get() != INVALID_HANDLE_VALUE, "fixture-file-create");
  DWORD written = 0;
  Require(WriteFile(file.get(), data, bytes, &written, nullptr) && written == bytes, "fixture-file-write");
}
std::vector<BYTE> Read(const std::wstring& path, DWORD maximum) {
  const Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  Require(file.get() != INVALID_HANDLE_VALUE, "fixture-file-read-open");
  BY_HANDLE_FILE_INFORMATION info{};
  Require(GetFileInformationByHandle(file.get(), &info) && info.nNumberOfLinks == 1 &&
      !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
      info.nFileSizeHigh == 0 && info.nFileSizeLow <= maximum, "fixture-file-read-type", ERROR_INVALID_DATA);
  std::vector<BYTE> bytes(info.nFileSizeLow);
  DWORD count = 0;
  Require(ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) &&
      count == bytes.size(), "fixture-file-read");
  return bytes;
}
std::vector<BYTE> User(HANDLE token) {
  alignas(TOKEN_USER) BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
  DWORD bytes = 0;
  Require(GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &bytes), "token-user");
  PSID sid = reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid;
  std::vector<BYTE> result(GetLengthSid(sid));
  Require(CopySid(static_cast<DWORD>(result.size()), result.data(), sid), "copy-token-user");
  return result;
}
std::vector<BYTE> RuntimeSid() {
  DWORD bytes = SECURITY_MAX_SID_SIZE, domain_bytes = 256;
  std::vector<BYTE> result(bytes);
  wchar_t domain[256]{};
  SID_NAME_USE use{};
  Require(LookupAccountNameW(nullptr, storage::kRuntimeIdentityAccount, result.data(), &bytes,
      domain, &domain_bytes, &use), "lookup-runtime-sid");
  result.resize(bytes);
  return result;
}
std::wstring SidText(PSID sid) {
  LPWSTR raw = nullptr;
  Require(ConvertSidToStringSidW(sid, &raw), "sid-string");
  const Local own(raw);
  return raw;
}
void AbsentRuntimeService() {
  const Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  Require(!!manager, "open-scm");
  const Service existing(OpenServiceW(manager.get(), storage::kRuntimeIdentityService, SERVICE_QUERY_STATUS));
  Require(!existing && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST,
      "refuse-preexisting-runtime-service", ERROR_ALREADY_EXISTS);
}
void AbsentRuntimeProfile(PSID sid) {
  const std::wstring path = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\" + SidText(sid);
  HKEY key = nullptr;
  const LSTATUS result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key);
  if (key) RegCloseKey(key);
  Require(result == ERROR_FILE_NOT_FOUND, "refuse-preexisting-runtime-profile",
      result == ERROR_SUCCESS ? ERROR_ALREADY_EXISTS : static_cast<DWORD>(result));
}
DWORD DeleteOwnedProfile(const std::wstring& directory) {
  const std::wstring marker = directory + L"\\profile.sid";
  if (GetFileAttributesW(marker.c_str()) == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
  }
  try {
    const auto sid = Read(marker, SECURITY_MAX_SID_SIZE);
    Require(sid.size() >= 8 && IsValidSid(const_cast<BYTE*>(sid.data())) &&
        GetLengthSid(const_cast<BYTE*>(sid.data())) == sid.size(), "owned-profile-marker", ERROR_INVALID_SID);
    const std::wstring text = SidText(const_cast<BYTE*>(sid.data()));
    if (!DeleteProfileW(text.c_str(), nullptr, nullptr)) {
      const DWORD error = GetLastError();
      if (error != ERROR_FILE_NOT_FOUND && error != ERROR_NOT_FOUND) return error;
    }
    return DeleteFileW(marker.c_str()) ? ERROR_SUCCESS : GetLastError();
  } catch (const Failure& failure) { return failure.status; }
}
void Denied(HANDLE desktop, const std::wstring& path, DWORD access) {
  Require(ImpersonateLoggedOnUser(desktop), "impersonate-desktop-negative");
  const HANDLE file = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  const DWORD error = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  if (!RevertToSelf()) std::abort();
  Require(error == ERROR_ACCESS_DENIED, "desktop-mutation-denied", error ? error : ERROR_ACCESS_DENIED);
}
void DesktopHelper(HANDLE desktop, HANDLE job, PSID user, PSID capability) {
  const Local descriptor = Security();
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor.get(), FALSE};
  std::wstring command = Quote(config.image) + L" --prepare-desktop " + Quote(SidText(user)) + L" " + Quote(SidText(capability));
  SIZE_T needed = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &needed);
  Require(needed != 0, "desktop-helper-attribute-size");
  std::vector<std::byte> memory(needed);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
  Require(InitializeProcThreadAttributeList(attributes, 1, 0, &needed), "desktop-helper-attributes");
  const auto release = [](LPPROC_THREAD_ATTRIBUTE_LIST value) { DeleteProcThreadAttributeList(value); };
  const std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(release)> own(attributes, release);
  HANDLE jobs[] = {job};
  Require(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs, sizeof(jobs), nullptr, nullptr),
      "desktop-helper-atomic-job");
  wchar_t desktop_name[] = L"winsta0\\default";
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.lpDesktop = desktop_name;
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION process{};
  Require(CreateProcessAsUserW(desktop, config.image.c_str(), command.data(), &security, &security,
      FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, config.directory.c_str(),
      &startup.StartupInfo, &process),
      "desktop-helper-create");
  const Handle child(process.hProcess), thread(process.hThread);
  BOOL contained = FALSE;
  DWORD setup = ERROR_SUCCESS;
  if (!IsProcessInJob(child.get(), job, &contained)) setup = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
  else if (!contained) setup = ERROR_ACCESS_DENIED;
  else if (ResumeThread(thread.get()) == MAXDWORD) setup = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
  if (setup != ERROR_SUCCESS) {
    const DWORD error = setup;
    TerminateProcess(child.get(), error);
    WaitForSingleObject(child.get(), 5000);
    throw Failure{error, "desktop-helper-job"};
  }
  const DWORD waited = WaitForSingleObject(child.get(), 10000);
  if (waited != WAIT_OBJECT_0) {
    TerminateProcess(child.get(), ERROR_TIMEOUT);
    WaitForSingleObject(child.get(), 5000);
    throw Failure{ERROR_TIMEOUT, "desktop-helper-wait"};
  }
  DWORD code = 0;
  Require(GetExitCodeProcess(child.get(), &code), "desktop-helper-exit");
  Check(code, "desktop-helper-result");
}
Evidence RunSystem() {
  Evidence evidence;
  bool installed = false, owns_profile = false;
  storage::ProtectedIdentityCleanup cleanup;
  std::unique_ptr<storage::ProtectedIdentity> identity;
  std::unique_ptr<storage::AuthorityLaunch> child;
  container::Sid package, capability;
  std::wstring package_name;
  const char* stage = "system.begin";
  try {
    AbsentRuntimeService();
    Check(storage::InstallRuntimeIdentityService(config.image), "install-owned-runtime-holder");
    installed = true;
    auto sid = RuntimeSid();
    AbsentRuntimeProfile(sid.data());
    Check(container::Grant(config.directory, sid.data(), FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, true),
        "private-holder-execute-acl");
    Check(container::Grant(config.image, sid.data(), FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false),
        "private-holder-image-acl");
    // This protected marker is written only after proving there is no earlier
    // profile. The controller can finish owned cleanup after a service timeout.
    WriteNew(config.directory + L"\\profile.sid", sid.data(), static_cast<DWORD>(sid.size()));
    owns_profile = true;
    const DWORD identity_status = storage::ProtectedIdentity::Create(config.image, config.session, &identity, &stage, &cleanup);
    Check(identity_status, stage);
    Require(EqualSid(identity->sid(), sid.data()), "actual-private-token-user", ERROR_ACCESS_DENIED);
    HANDLE raw = nullptr;
    Require(WTSQueryUserToken(config.session, &raw), "actual-wts-desktop-token");
    const Handle desktop(raw);
    BYTE admin_sid[SECURITY_MAX_SID_SIZE]{};
    DWORD admin_bytes = sizeof(admin_sid);
    Require(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin_sid, &admin_bytes), "negative-admin-sid");
    SID_AND_ATTRIBUTES disabled{admin_sid, 0};
    HANDLE filtered_raw = nullptr;
    Require(CreateRestrictedToken(desktop.get(), DISABLE_MAX_PRIVILEGE, 1, &disabled,
        0, nullptr, 0, nullptr, &filtered_raw), "filter-ordinary-desktop-token");
    const Handle filtered(filtered_raw);
    auto desktop_sid = User(desktop.get());
    PSID desktop_user = desktop_sid.data();
    Require(!EqualSid(desktop_user, identity->sid()), "distinct-desktop-and-runtime-user", ERROR_ACCESS_DENIED);
    for (DWORD access : {static_cast<DWORD>(FILE_WRITE_DATA), static_cast<DWORD>(WRITE_DAC), static_cast<DWORD>(DELETE)}) {
      Denied(filtered.get(), config.image, access);
    }
    package_name = L"Gesher.LaunchTest." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    Require(ImpersonateLoggedOnUser(identity->token()), "impersonate-private-profile");
    const DWORD profile = container::EnsureProfile(package_name, package_name, &package);
    if (!RevertToSelf()) std::abort();
    Check(profile, "create-owned-appcontainer");
    Check(container::CapabilitySid(container::kRuntimeFilesCapability, &capability), "runtime-file-capability");
    for (PSID reader : {identity->sid(), capability.value, desktop_user}) {
      Check(container::Grant(config.directory, reader, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, true), "fixture-execute-acl");
      Check(container::Grant(config.image, reader, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false), "fixture-image-execute-acl");
    }
    DesktopHelper(desktop.get(), service_job.get(), identity->sid(), capability.value);
    const std::wstring temporary = config.directory + L"\\private-data";
    PrivateDirectory(temporary, true);
    Check(container::Grant(temporary, identity->sid(), FILE_ALL_ACCESS, true), "private-data-user-acl");
    Check(container::Grant(temporary, package.value, FILE_ALL_ACCESS, true), "private-data-lowbox-acl");
    Denied(filtered.get(), temporary, FILE_ADD_FILE);
    wchar_t windows[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
    Require(length && length < MAX_PATH, "windows-directory");
    std::vector<std::wstring> values{L"SystemRoot=" + std::wstring(windows), L"WINDIR=" + std::wstring(windows),
        L"PATH=" + config.directory + L";" + windows + L"\\System32"};
    Check(identity->AppendProfileEnvironment(&values), "actual-private-profile-environment");
    std::vector<std::wstring> duplicate{L"localappdata=C:\\untrusted"};
    Require(identity->AppendProfileEnvironment(&duplicate) == ERROR_INVALID_PARAMETER,
        "deny-profile-environment-override", ERROR_INVALID_DATA);
    wchar_t profile_buffer[MAX_PATH]{};
    DWORD profile_size = static_cast<DWORD>(std::size(profile_buffer));
    Require(GetUserProfileDirectoryW(identity->token(), profile_buffer, &profile_size), "independent-private-profile-path");
    const std::wstring profile_path(profile_buffer);
    bool matched_profile = false;
    for (const auto& value : values) {
      if (value == L"USERPROFILE=" + profile_path) matched_profile = true;
    }
    Require(matched_profile, "private-environment-matches-token-profile", ERROR_INVALID_DATA);
    values.push_back(L"TEMP=" + temporary);
    values.push_back(L"TMP=" + temporary);
    std::wstring environment;
    for (const auto& value : values) { environment += value; environment.push_back(L'\0'); }
    environment.push_back(L'\0');
    SID_AND_ATTRIBUTES caps[] = {{capability.value, SE_GROUP_ENABLED}};
    const std::wstring arguments = L"--guest " + Quote(profile_path);
    const storage::AuthorityLaunchSpec spec{config.image.c_str(), config.directory.c_str(), arguments,
        environment, {package.value, caps, 1, 0}, identity->sid(), config.session, &cleanup.profile_error, service_job.get()};
    const DWORD created = storage::AuthorityLaunch::Create(identity->token(), spec, &child, &stage);
    Check(created, stage);
    Require(WaitForSingleObject(child->process(), 0) == WAIT_TIMEOUT, "exact-child-suspended");
    BOOL contained = FALSE;
    Require(IsProcessInJob(child->process(), service_job.get(), &contained) && contained, "inherited-lowbox-job");
    const DWORD mutations[] = {PROCESS_VM_WRITE, PROCESS_VM_OPERATION, PROCESS_DUP_HANDLE,
        PROCESS_TERMINATE, PROCESS_SUSPEND_RESUME, PROCESS_CREATE_THREAD, WRITE_DAC, WRITE_OWNER};
    for (DWORD access : mutations) {
      Require(ImpersonateLoggedOnUser(filtered.get()), "impersonate-process-negative");
      const HANDLE impostor = OpenProcess(access, FALSE, child->pid());
      const DWORD denied = impostor ? ERROR_SUCCESS : GetLastError();
      if (impostor) CloseHandle(impostor);
      if (!RevertToSelf()) std::abort();
      Require(denied == ERROR_ACCESS_DENIED, "desktop-child-mutation-denied", denied ? denied : ERROR_ACCESS_DENIED);
    }
    Check(child->Resume(), "resume-exact-lowbox");
    Require(WaitForSingleObject(child->process(), 10000) == WAIT_OBJECT_0, "guest-completion", ERROR_TIMEOUT);
    DWORD code = 0;
    Require(GetExitCodeProcess(child->process(), &code), "guest-exit-query");
    Check(code, "guest-private-environment-witness");
    Check(child->Stop(0), "idempotent-exited-stop");
    Require(child->Resume() == ERROR_PROCESS_ABORTED, "deny-resume-after-stop", ERROR_INVALID_DATA);
  } catch (const Failure& failure) {
    evidence.status = failure.status;
    std::snprintf(evidence.stage, sizeof(evidence.stage), "%s", failure.stage);
  } catch (...) { evidence.status = ERROR_NOT_ENOUGH_MEMORY; }
  if (child) {
    const DWORD stopped = child->Stop(10000);
    if (stopped != ERROR_SUCCESS) evidence.cleanup = stopped;
    child.reset();
  }
  if (cleanup.profile_error.load() != ERROR_SUCCESS && evidence.cleanup == ERROR_SUCCESS) {
    evidence.cleanup = cleanup.profile_error.load();
  }
  if (package.value && identity && evidence.cleanup == ERROR_SUCCESS) {
    if (ImpersonateLoggedOnUser(identity->token())) {
      const DWORD deleted = container::DeleteContainerProfile(package_name);
      if (!RevertToSelf()) std::abort();
      if (deleted != ERROR_SUCCESS) evidence.cleanup = deleted;
    } else evidence.cleanup = GetLastError();
  }
  if (identity) {
    const DWORD unloaded = identity->UnloadProfile(evidence.cleanup);
    if (unloaded != ERROR_SUCCESS && evidence.cleanup == ERROR_SUCCESS) evidence.cleanup = unloaded;
    identity.reset();
  }
  if (installed) {
    const DWORD removed = storage::UninstallRuntimeIdentityService(config.image);
    if (removed != ERROR_SUCCESS && evidence.cleanup == ERROR_SUCCESS) evidence.cleanup = removed;
  }
  if (owns_profile && evidence.cleanup == ERROR_SUCCESS) evidence.cleanup = DeleteOwnedProfile(config.directory);
  if (cleanup.profile_error.load() != ERROR_SUCCESS) evidence.cleanup = cleanup.profile_error.load();
  if (cleanup.privilege_restore_error != ERROR_SUCCESS) evidence.cleanup = cleanup.privilege_restore_error;
  return evidence;
}
void Report(DWORD state, DWORD error = ERROR_SUCCESS) {
  SERVICE_STATUS status{};
  status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  status.dwCurrentState = state;
  status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
  status.dwWin32ExitCode = error;
  SetServiceStatus(service_status, &status);
}
DWORD WINAPI Control(DWORD operation, DWORD, void*, void*) {
  if (operation == SERVICE_CONTROL_STOP) { SetEvent(service_stop.get()); return ERROR_SUCCESS; }
  return operation == SERVICE_CONTROL_INTERROGATE ? ERROR_SUCCESS : ERROR_CALL_NOT_IMPLEMENTED;
}
void WINAPI ServiceMain(DWORD, wchar_t**) {
  service_stop.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  service_status = RegisterServiceCtrlHandlerExW(config.name.c_str(), Control, nullptr);
  if (!service_status) return;
  try {
    // Only this disposable test process grants the elevated controller a
    // retained termination handle for its independent timeout watchdog.
    const Local descriptor = Security();
    PACL acl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    Require(GetSecurityDescriptorDacl(descriptor.get(), &present, &acl, &defaulted) && present && acl,
        "system-service-controller-acl");
    Check(SetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr), "system-service-controller-access");
    // The independently created controller job is exclusive and SY/BA-only.
    // Keep the Session0 service outside this desktop-session job. Every child
    // is assigned at creation through JOB_LIST; no job handle is inherited.
    service_job.reset(OpenJobObjectW(JOB_OBJECT_ALL_ACCESS, FALSE,
        (L"Global\\" + config.name).c_str()));
    Require(!!service_job, "open-owned-desktop-job");
  } catch (const Failure& failure) {
    Evidence evidence;
    evidence.status = failure.status;
    std::snprintf(evidence.stage, sizeof(evidence.stage), "%s", failure.stage);
    try { WriteNew(config.directory + L"\\result.bin", &evidence, sizeof(evidence)); } catch (...) {}
    Report(SERVICE_STOPPED, failure.status);
    return;
  }
  Report(SERVICE_RUNNING);
  Evidence evidence = RunSystem();
  try { WriteNew(config.directory + L"\\result.bin", &evidence, sizeof(evidence)); }
  catch (const Failure& failure) { evidence.status = failure.status; }
  Report(SERVICE_STOPPED, evidence.status ? evidence.status : evidence.cleanup);
}
DWORD Guest(const wchar_t* profile) {
  wchar_t value[8193]{};
  for (const wchar_t* name : {L"USERPROFILE", L"LOCALAPPDATA", L"APPDATA"}) {
    const DWORD size = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
    if (!size || size >= std::size(value)) return ERROR_INVALID_DATA;
    const size_t prefix = std::wcslen(profile);
    if (size < prefix || CompareStringOrdinal(value, static_cast<int>(prefix), profile,
        static_cast<int>(prefix), TRUE) != CSTR_EQUAL || (size > prefix && value[prefix] != L'\\')) return ERROR_ACCESS_DENIED;
  }
  const DWORD count = GetEnvironmentVariableW(L"TEMP", value, static_cast<DWORD>(std::size(value)));
  if (!count || count >= std::size(value)) return ERROR_INVALID_DATA;
  const std::wstring path = std::wstring(value) + L"\\guest.bin";
  const Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) return GetLastError();
  DWORD written = 0;
  if (!WriteFile(file.get(), "GSH", 3, &written, nullptr)) {
    const DWORD error = GetLastError();
    return error ? error : ERROR_WRITE_FAULT;
  }
  return written == 3 ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
}
DWORD Controller() {
  DWORD result = ERROR_SUCCESS;
  Service service;
  Handle process;
  Handle job;
  std::wstring directory, image;
  bool own_directory = false;
  try {
    BOOL admin = FALSE;
    BYTE admin_sid[SECURITY_MAX_SID_SIZE]{};
    DWORD sid_bytes = sizeof(admin_sid);
    Require(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin_sid, &sid_bytes) &&
        CheckTokenMembership(nullptr, admin_sid, &admin) && admin, "require-elevated-controller", ERROR_ELEVATION_REQUIRED);
    AbsentRuntimeService();
    DWORD session = 0;
    Require(ProcessIdToSessionId(GetCurrentProcessId(), &session) && session != 0,
        "require-real-desktop-session", ERROR_NOT_READY);
    LPWSTR info = nullptr;
    DWORD size = 0;
    Require(WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSConnectState, &info, &size), "query-wts-state");
    const bool active = size == sizeof(WTS_CONNECTSTATE_CLASS) && *reinterpret_cast<WTS_CONNECTSTATE_CLASS*>(info) == WTSActive;
    WTSFreeMemory(info);
    Require(active, "require-active-wts-session", ERROR_NOT_READY);
    storage::ProtectedIdentityCleanup denied_cleanup;
    std::unique_ptr<storage::ProtectedIdentity> denied_identity;
    const char* denied_stage = "none";
    const DWORD denied = storage::ProtectedIdentity::Create(Image(), session, &denied_identity,
        &denied_stage, &denied_cleanup);
    Require(denied == ERROR_ACCESS_DENIED && !denied_identity, "reject-desktop-authority-impostor", ERROR_ACCESS_DENIED);
    PWSTR program_files = nullptr;
    const HRESULT root = SHGetKnownFolderPath(FOLDERID_ProgramFilesX64, 0, nullptr, &program_files);
    Require(root == S_OK && program_files, "program-files-root", HRESULT_CODE(root));
    directory = std::wstring(program_files) + L"\\Gesher protected-launch-test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    CoTaskMemFree(program_files);
    PrivateDirectory(directory);
    own_directory = true;
    image = directory + L"\\probe.exe";
    const auto binary = Read(Image(), 64 * 1024 * 1024);
    WriteNew(image, binary.data(), static_cast<DWORD>(binary.size()));
    const Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    Require(!!manager, "controller-open-scm");
    const std::wstring name = L"GesherLaunchTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const Local descriptor = Security();
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor.get(), FALSE};
    job.reset(CreateJobObjectW(&security, (L"Global\\" + name).c_str()));
    const DWORD job_error = GetLastError();
    const bool own_job = job && job_error != ERROR_ALREADY_EXISTS;
    if (!own_job) job.reset();
    Require(own_job, "exclusive-desktop-job",
        job_error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : job_error);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
        "configure-desktop-job");
    const std::wstring command = Quote(image) + L" --test-service " + name + L" " + std::to_wstring(session);
    service.reset(CreateServiceW(manager.get(), name.c_str(), name.c_str(), SERVICE_START | SERVICE_STOP |
        SERVICE_QUERY_STATUS | DELETE, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
        command.c_str(), nullptr, nullptr, nullptr, L"LocalSystem", nullptr));
    Require(!!service, "create-owned-system-service");
    Require(StartServiceW(service.get(), 0, nullptr), "start-owned-system-service");
    const ULONGLONG deadline = GetTickCount64() + 90000;
    for (;;) {
      SERVICE_STATUS_PROCESS state{};
      DWORD bytes = 0;
      Require(QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&state),
          sizeof(state), &bytes), "query-owned-system-service");
      if (!process && state.dwProcessId && state.dwCurrentState == SERVICE_RUNNING) {
        process.reset(OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, state.dwProcessId));
        Require(!!process, "retain-exact-system-service");
        wchar_t actual[32768]{};
        DWORD capacity = static_cast<DWORD>(std::size(actual));
        Require(QueryFullProcessImageNameW(process.get(), 0, actual, &capacity) &&
            CompareStringOrdinal(actual, static_cast<int>(capacity), image.c_str(), static_cast<int>(image.size()), TRUE) == CSTR_EQUAL,
            "verify-system-service-image", ERROR_ACCESS_DENIED);
      }
      if (state.dwCurrentState == SERVICE_STOPPED) break;
      Require(GetTickCount64() < deadline, "bounded-system-service-run", ERROR_TIMEOUT);
      Sleep(25);
    }
    const auto data = Read(directory + L"\\result.bin", sizeof(Evidence));
    Require(data.size() == sizeof(Evidence), "complete-test-evidence", ERROR_INVALID_DATA);
    Evidence evidence{};
    std::memcpy(&evidence, data.data(), sizeof(evidence));
    Require(evidence.magic == 0x47504c54 && std::memchr(evidence.stage, 0, sizeof(evidence.stage)),
        "validate-test-evidence", ERROR_INVALID_DATA);
    std::printf("Protected launch: stage=%s win32=%lu cleanup_win32=%lu\n", evidence.stage,
        static_cast<unsigned long>(evidence.status), static_cast<unsigned long>(evidence.cleanup));
    result = evidence.status ? evidence.status : evidence.cleanup;
  } catch (const Failure& failure) {
    std::fprintf(stderr, "Protected launch: stage=%s win32=%lu\n", failure.stage, static_cast<unsigned long>(failure.status));
    result = failure.status;
  } catch (...) { result = ERROR_NOT_ENOUGH_MEMORY; }
  DWORD cleanup_error = ERROR_SUCCESS;
  bool worker_stopped = !service;
  if (service) {
    SERVICE_STATUS state{};
    ControlService(service.get(), SERVICE_CONTROL_STOP, &state);
    if (process && WaitForSingleObject(process.get(), 5000) != WAIT_OBJECT_0) {
      if (!TerminateProcess(process.get(), ERROR_TIMEOUT)) cleanup_error = GetLastError();
      else if (WaitForSingleObject(process.get(), 5000) != WAIT_OBJECT_0) cleanup_error = ERROR_TIMEOUT;
    }
    if (process) worker_stopped = WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0;
    else {
      SERVICE_STATUS_PROCESS final{};
      DWORD bytes = 0;
      worker_stopped = QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
          reinterpret_cast<BYTE*>(&final), sizeof(final), &bytes) && final.dwCurrentState == SERVICE_STOPPED;
    }
    if (!worker_stopped && cleanup_error == ERROR_SUCCESS) cleanup_error = ERROR_TIMEOUT;
    if (!DeleteService(service.get()) && cleanup_error == ERROR_SUCCESS) cleanup_error = GetLastError();
    service.reset();
    process.reset();
  }
  bool job_empty = !job;
  if (job) {
    if (!TerminateJobObject(job.get(), ERROR_OPERATION_ABORTED) && cleanup_error == ERROR_SUCCESS) cleanup_error = GetLastError();
    const ULONGLONG deadline = GetTickCount64() + 10000;
    do {
      JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
      if (!QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
        if (cleanup_error == ERROR_SUCCESS) cleanup_error = GetLastError();
        break;
      }
      if (accounting.ActiveProcesses == 0) { job_empty = true; break; }
      Sleep(25);
    } while (GetTickCount64() < deadline);
    if (!job_empty && cleanup_error == ERROR_SUCCESS) cleanup_error = ERROR_TIMEOUT;
  }
  if (own_directory && worker_stopped && job_empty) {
    const DWORD removed = storage::UninstallRuntimeIdentityService(image);
    if (removed != ERROR_SUCCESS && removed != ERROR_SERVICE_DOES_NOT_EXIST && cleanup_error == ERROR_SUCCESS) cleanup_error = removed;
    const DWORD profile = DeleteOwnedProfile(directory);
    if (profile != ERROR_SUCCESS && cleanup_error == ERROR_SUCCESS) cleanup_error = profile;
    DeleteFileW((directory + L"\\private-data\\guest.bin").c_str());
    RemoveDirectoryW((directory + L"\\private-data").c_str());
    DeleteFileW((directory + L"\\result.bin").c_str());
    if (!DeleteFileW(image.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND && cleanup_error == ERROR_SUCCESS) cleanup_error = GetLastError();
    if (!RemoveDirectoryW(directory.c_str()) && cleanup_error == ERROR_SUCCESS) cleanup_error = GetLastError();
  }
  if (cleanup_error != ERROR_SUCCESS) std::fprintf(stderr, "Protected launch: controller-cleanup=%lu\n", static_cast<unsigned long>(cleanup_error));
  if (result == ERROR_SUCCESS) result = cleanup_error;
  if (result == ERROR_SUCCESS) std::puts("PASS: actual private SCM identity, profile environment, lowbox child, desktop denial and bounded cleanup");
  return result;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 2 && std::wstring_view(argv[1]) == L"runtime-identity") return storage::RunRuntimeIdentityService();
  if (argc == 4 && std::wstring_view(argv[1]) == L"--prepare-desktop") return static_cast<int>(storage::PrepareRuntimeDesktop(argv[2], argv[3]));
  if (argc == 3 && std::wstring_view(argv[1]) == L"--guest") return static_cast<int>(Guest(argv[2]));
  if (argc == 4 && std::wstring_view(argv[1]) == L"--test-service") {
    try {
      config.image = Image();
      config.directory = config.image.substr(0, config.image.find_last_of(L'\\'));
      config.name = argv[2];
      wchar_t* end = nullptr;
      const unsigned long session = std::wcstoul(argv[3], &end, 10);
      Require(session && session < MAXDWORD && end && !*end, "test-session-argument", ERROR_INVALID_PARAMETER);
      config.session = static_cast<DWORD>(session);
      SERVICE_TABLE_ENTRYW entries[] = {{config.name.data(), ServiceMain}, {nullptr, nullptr}};
      return StartServiceCtrlDispatcherW(entries) ? 0 : static_cast<int>(GetLastError());
    } catch (...) { return 1; }
  }
  return argc == 1 ? static_cast<int>(Controller()) : 2;
}
