// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <wtsapi32.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../framework/app_container.h"
#include "authority_launch.h"
#include "binder_authority_control.h"
#include "binder_listener_factory.h"

namespace {

namespace appcontainer = gesher::appcontainer;
using gesher::usb_storage::AuthorityLaunch;
using gesher::usb_storage::AuthorityLaunchSpec;
using gesher::usb_storage::BinderAuthorityControl;
using gesher::usb_storage::BinderListenerFactory;

constexpr wchar_t kService[] = L"GesherUsbAuthority";
constexpr wchar_t kFolder[] = L"Gesher Raw USB Broker";
constexpr wchar_t kImage[] = L"gesher-usb-authority.exe";
constexpr DWORD kStopTimeoutMilliseconds = 30'000;
constexpr wchar_t kTrustedInstaller[] =
    L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

class ScopedHandle {
 public:
  explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
  ~ScopedHandle() {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  }
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  HANDLE get() const { return handle_; }
 private:
  HANDLE handle_;
};

class ScHandle {
 public:
  explicit ScHandle(SC_HANDLE handle) : handle_(handle) {}
  ~ScHandle() { if (handle_) CloseServiceHandle(handle_); }
  ScHandle(const ScHandle&) = delete;
  ScHandle& operator=(const ScHandle&) = delete;
  SC_HANDLE get() const { return handle_; }
 private:
  SC_HANDLE handle_;
};

bool EqualsPath(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool Elevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  ScopedHandle close(token);
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
  PSID administrators = nullptr;
  if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                               DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                               &administrators)) return false;
  BOOL member = FALSE;
  const bool authorized = CheckTokenMembership(nullptr, administrators, &member) && member &&
      GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
      elevation.TokenIsElevated;
  FreeSid(administrators);
  return authorized;
}

bool IsLocalSystem() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  ScopedHandle close(token);
  alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
  DWORD size = 0;
  return GetTokenInformation(token, TokenUser, user, sizeof(user), &size) &&
      IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(user)->User.Sid, WinLocalSystemSid);
}

bool ProtectedDefaultOwner() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  ScopedHandle close(token);
  alignas(TOKEN_OWNER) BYTE owner[sizeof(TOKEN_OWNER) + SECURITY_MAX_SID_SIZE]{};
  DWORD size = 0;
  if (!GetTokenInformation(token, TokenOwner, owner, sizeof(owner), &size)) return false;
  PSID sid = reinterpret_cast<TOKEN_OWNER*>(owner)->Owner;
  return IsWellKnownSid(sid, WinBuiltinAdministratorsSid) || IsWellKnownSid(sid, WinLocalSystemSid);
}

bool TrustedWriter(PSID sid, PSID installer) {
  return sid && (IsWellKnownSid(sid, WinLocalSystemSid) ||
      IsWellKnownSid(sid, WinBuiltinAdministratorsSid) || (installer && EqualSid(sid, installer)));
}

// Reject mutation rights for all principals except the machine installers.
// Explicit deny ACEs do not make an ambiguous allow safe; use a known ACL
// rather than making correctness depend on ACE ordering or group membership.
bool ProtectedAcl(PACL dacl, PSID owner, PSID installer, DWORD mutation) {
  if (!dacl || !TrustedWriter(owner, installer)) return false;
  mutation |= GENERIC_ALL | GENERIC_WRITE | MAXIMUM_ALLOWED | DELETE | WRITE_DAC | WRITE_OWNER;
  for (DWORD i = 0; i < dacl->AceCount; ++i) {
    void* raw = nullptr;
    if (!GetAce(dacl, i, &raw)) return false;
    const auto* header = static_cast<const ACE_HEADER*>(raw);
    if (header->AceFlags & INHERIT_ONLY_ACE) continue;
    if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
    auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
    if ((ace->Mask & mutation) && !TrustedWriter(&ace->SidStart, installer)) return false;
  }
  return true;
}

bool ProtectedFileAcl(HANDLE object, PSID installer, bool restrict_creation) {
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (GetSecurityInfo(object, SE_FILE_OBJECT,
                      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                      &owner, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
  DWORD mutation = FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD;
  if (restrict_creation) mutation |= FILE_WRITE_DATA | FILE_APPEND_DATA;
  const bool protected_acl = ProtectedAcl(dacl, owner, installer, mutation);
  LocalFree(descriptor);
  return protected_acl;
}

bool ProtectedFile(const std::wstring& path, bool directory, PSID installer,
                   bool restrict_creation = true) {
  // Open the object itself, including reparse points, and deny write/delete
  // sharing while checking its type, canonical path and security descriptor.
  ScopedHandle object(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
                                 FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (object.get() == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(object.get(), &info) ||
      (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
      !!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory ||
      (!directory && info.nNumberOfLinks != 1)) return false;
  constexpr DWORD kCapacity = 32768;
  std::wstring canonical(kCapacity, L'\0');
  const DWORD length = GetFinalPathNameByHandleW(object.get(), canonical.data(), kCapacity,
                                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!length || length >= kCapacity) return false;
  canonical.resize(length);
  return EqualsPath(canonical, L"\\\\?\\" + path) &&
      ProtectedFileAcl(object.get(), installer, restrict_creation);
}

DWORD TrustedImage(std::wstring* image, std::wstring* application_directory = nullptr) {
  PWSTR program_files = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX64, KF_FLAG_DONT_VERIFY,
                                  nullptr, &program_files))) return ERROR_PATH_NOT_FOUND;
  const std::wstring root(program_files);
  CoTaskMemFree(program_files);
  if (root.size() < 4 || root[1] != L':' || root[2] != L'\\' ||
      GetDriveTypeW(root.substr(0, 3).c_str()) != DRIVE_FIXED) return ERROR_ACCESS_DENIED;
  const std::wstring directory = root + L"\\" + kFolder;
  const std::wstring expected = directory + L"\\" + kImage;
  constexpr DWORD kCapacity = 32768;
  std::wstring running(kCapacity, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, running.data(), kCapacity);
  if (!length || length >= kCapacity) return ERROR_ACCESS_DENIED;
  running.resize(length);
  if (!EqualsPath(running, expected)) return ERROR_ACCESS_DENIED;

  PSID installer = nullptr;
  if (!ConvertStringSidToSidW(kTrustedInstaller, &installer)) return GetLastError();
  bool trusted = ProtectedFile(root.substr(0, 3), true, installer, false);
  for (size_t i = 3; trusted && i <= directory.size(); ++i) {
    if (i == directory.size() || directory[i] == L'\\') {
      trusted = ProtectedFile(directory.substr(0, i), true, installer, i >= root.size());
    }
  }
  if (trusted) trusted = ProtectedFile(expected, false, installer);
  LocalFree(installer);
  if (!trusted) return ERROR_ACCESS_DENIED;
  *image = L"\"" + expected + L"\" service";
  if (application_directory) *application_directory = directory;
  return ERROR_SUCCESS;
}

bool ProtectedRuntimeTree(const std::wstring& directory, PSID installer,
                          unsigned depth, unsigned* remaining) {
  if (depth == 0 || !ProtectedFile(directory, true, installer)) return false;
  WIN32_FIND_DATAW entry{};
  HANDLE search = FindFirstFileW((directory + L"\\*").c_str(), &entry);
  if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
  bool protected_tree = true;
  do {
    if (std::wcscmp(entry.cFileName, L".") == 0 || std::wcscmp(entry.cFileName, L"..") == 0) continue;
    if (*remaining == 0 || (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      protected_tree = false;
      break;
    }
    --*remaining;
    const std::wstring child = directory + L"\\" + entry.cFileName;
    protected_tree = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        ? ProtectedRuntimeTree(child, installer, depth - 1, remaining)
        : ProtectedFile(child, false, installer);
  } while (protected_tree && FindNextFileW(search, &entry));
  const bool end = protected_tree && GetLastError() == ERROR_NO_MORE_FILES;
  FindClose(search);
  return end;
}

DWORD ValidateRuntime(const std::wstring& directory) {
  const std::wstring runtime = directory + L"\\runtime";
  PSID installer = nullptr;
  if (!ConvertStringSidToSidW(kTrustedInstaller, &installer)) return GetLastError();
  unsigned remaining = 1024;
  const bool protected_tree = ProtectedRuntimeTree(runtime, installer, 24, &remaining);
  LocalFree(installer);
  if (!protected_tree) return ERROR_ACCESS_DENIED;
  // The elevated installer verifies every packaged digest. Check the core boot
  // files also exist before announcing the installed authority as ready.
  constexpr const wchar_t* required[] = {
      L"dalvikvm.exe", L"binderd.exe", L"servicemanager.exe", L"gesher-exec.exe",
      L"core-oj.jar", L"core-libart.jar", L"core-icu4j.jar", L"framework.jar",
      L"gesher-app.jar", L"framework\\x86_64\\boot.art", L"framework\\x86_64\\boot.oat",
      L"framework\\x86_64\\boot.vdex", L"system\\framework\\framework-res.apk",
      L"system\\build.prop", L"i18n\\etc\\icu\\icudt76l.dat", L"tzdata\\etc\\tz\\tzdata",
      L"libgesher_webview_bridge.so"};
  for (const wchar_t* name : required) {
    const DWORD attributes = GetFileAttributesW((runtime + L"\\" + name).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError();
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return ERROR_INVALID_DATA;
  }
  return ERROR_SUCCESS;
}

DWORD ValidateRegistration(SC_HANDLE service, const std::wstring& image) {
  DWORD bytes = 0;
  constexpr SECURITY_INFORMATION kOwnerAndDacl = OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
  if (QueryServiceObjectSecurity(service, kOwnerAndDacl, nullptr, 0, &bytes) ||
      GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes > 8192) return ERROR_INVALID_DATA;
  std::vector<BYTE> security(bytes);
  if (!QueryServiceObjectSecurity(service, kOwnerAndDacl, security.data(), bytes, &bytes)) {
    return GetLastError();
  }
  PSID owner = nullptr;
  PACL dacl = nullptr;
  BOOL owner_defaulted = FALSE, present = FALSE, defaulted = FALSE;
  constexpr DWORD kControl = SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_STOP |
      SERVICE_PAUSE_CONTINUE | SERVICE_USER_DEFINED_CONTROL;
  if (!GetSecurityDescriptorOwner(security.data(), &owner, &owner_defaulted) ||
      !GetSecurityDescriptorDacl(security.data(), &present, &dacl, &defaulted) ||
      !present || !ProtectedAcl(dacl, owner, nullptr, kControl)) return ERROR_ACCESS_DENIED;

  DWORD needed = 0;
  if (QueryServiceConfigW(service, nullptr, 0, &needed) ||
      GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed > 8192) return ERROR_INVALID_DATA;
  std::vector<BYTE> data(needed);
  auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(data.data());
  if (!QueryServiceConfigW(service, config, needed, &needed)) return GetLastError();
  if (!config->lpBinaryPathName || !config->lpServiceStartName ||
      !EqualsPath(config->lpBinaryPathName, image) ||
      config->dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
      config->dwStartType != SERVICE_DEMAND_START ||
      !EqualsPath(config->lpServiceStartName, L"LocalSystem")) return ERROR_INVALID_DATA;
  return ERROR_SUCCESS;
}

DWORD Install(const std::wstring& image) {
  if (!ProtectedDefaultOwner()) return ERROR_ACCESS_DENIED;
  ScHandle manager(OpenSCManagerW(nullptr, nullptr,
                                 SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
  if (!manager.get()) return GetLastError();
  ScHandle created(CreateServiceW(manager.get(), kService, L"Gesher raw USB authority",
                                  WRITE_DAC | WRITE_OWNER | DELETE, SERVICE_WIN32_OWN_PROCESS,
                                  SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
                                  image.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  if (created.get()) {
    // Keep service configuration, start and stop with the elevated principals.
    // Authenticated users can query it without obtaining any service control.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    constexpr wchar_t policy[] = L"O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;CCLCSWLORC;;;AU)";
    DWORD error = ERROR_SUCCESS;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            policy, SDDL_REVISION_1, &descriptor, nullptr)) {
      error = GetLastError();
    } else {
      if (!SetServiceObjectSecurity(created.get(),
                                    OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                    descriptor)) error = GetLastError();
      LocalFree(descriptor);
    }
    if (error != ERROR_SUCCESS) DeleteService(created.get());
    return error;
  }
  DWORD error = GetLastError();
  if (error != ERROR_SERVICE_EXISTS) return error;
  ScHandle existing(OpenServiceW(manager.get(), kService, SERVICE_QUERY_CONFIG | READ_CONTROL));
  return existing.get() ? ValidateRegistration(existing.get(), image) : GetLastError();
}

DWORD Uninstall(const std::wstring& image) {
  ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!manager.get()) return GetLastError();
  ScHandle service(OpenServiceW(manager.get(), kService, SERVICE_QUERY_CONFIG |
                               SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE | READ_CONTROL));
  if (!service.get()) {
    const DWORD error = GetLastError();
    return error == ERROR_SERVICE_DOES_NOT_EXIST ? ERROR_SUCCESS : error;
  }
  const DWORD validated = ValidateRegistration(service.get(), image);
  if (validated != ERROR_SUCCESS) return validated;
  SERVICE_STATUS_PROCESS process{};
  DWORD required = 0;
  if (!QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                            reinterpret_cast<BYTE*>(&process), sizeof(process), &required)) {
    return GetLastError();
  }
  // Retain the actual running process before sending STOP. A service that has
  // reported STOPPED can still be returning from its main routine.
  ScopedHandle running(process.dwProcessId != 0 && process.dwCurrentState != SERVICE_STOPPED
                           ? OpenProcess(SYNCHRONIZE, FALSE, process.dwProcessId) : nullptr);
  SERVICE_STATUS status{};
  status.dwCurrentState = process.dwCurrentState;
  if (status.dwCurrentState != SERVICE_STOPPED && status.dwCurrentState != SERVICE_STOP_PENDING) {
    if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &status) &&
        GetLastError() != ERROR_SERVICE_NOT_ACTIVE) return GetLastError();
  }
  const ULONGLONG deadline = GetTickCount64() + kStopTimeoutMilliseconds;
  while (status.dwCurrentState != SERVICE_STOPPED) {
    if (GetTickCount64() >= deadline) return ERROR_TIMEOUT;
    Sleep(100);
    if (!QueryServiceStatus(service.get(), &status)) return GetLastError();
  }
  if (running.get()) {
    const auto now = GetTickCount64();
    const DWORD waited = WaitForSingleObject(running.get(),
        now < deadline ? static_cast<DWORD>(deadline - now) : 0);
    if (waited != WAIT_OBJECT_0) return waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
  }
  return DeleteService(service.get()) ? ERROR_SUCCESS : GetLastError();
}

DWORD ClearInterruptedBootstrapQueries() {
  // A crash between creation and retirement of a bootstrap query ACE cannot
  // close an SCM DACL with a process handle. Clean that service-owned entry
  // before exposing the new service instance.
  ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!manager.get()) return GetLastError();
  ScHandle service(OpenServiceW(manager.get(), kService, READ_CONTROL | WRITE_DAC));
  if (!service.get()) return GetLastError();
  DWORD bytes = 0;
  if (QueryServiceObjectSecurity(service.get(), DACL_SECURITY_INFORMATION, nullptr, 0, &bytes) ||
      GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0 || bytes > 64 * 1024) {
    return ERROR_INVALID_DATA;
  }
  std::vector<BYTE> security(bytes);
  if (!QueryServiceObjectSecurity(service.get(), DACL_SECURITY_INFORMATION,
                                  security.data(), bytes, &bytes)) return GetLastError();
  BOOL present = FALSE, defaulted = FALSE;
  PACL acl = nullptr;
  if (!GetSecurityDescriptorDacl(security.data(), &present, &acl, &defaulted) ||
      !present || !acl || !IsValidAcl(acl)) return ERROR_INVALID_ACL;
  constexpr SID_IDENTIFIER_AUTHORITY kPackageAuthority = SECURITY_APP_PACKAGE_AUTHORITY;
  constexpr DWORD kTemporaryAccess = SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS;
  bool changed = false;
  for (DWORD i = acl->AceCount; i != 0; --i) {
    void* raw = nullptr;
    if (!GetAce(acl, i - 1, &raw)) return GetLastError();
    auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
    if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Header.AceFlags != 0 ||
        ace->Mask != kTemporaryAccess) continue;
    PSID sid = &ace->SidStart;
    if (!IsValidSid(sid) || *GetSidSubAuthorityCount(sid) <= 2 ||
        std::memcmp(GetSidIdentifierAuthority(sid), &kPackageAuthority,
                    sizeof(kPackageAuthority)) != 0 ||
        *GetSidSubAuthority(sid, 0) != SECURITY_APP_PACKAGE_BASE_RID) continue;
    if (!DeleteAce(acl, i - 1)) return GetLastError();
    changed = true;
  }
  return !changed || SetServiceObjectSecurity(service.get(), DACL_SECURITY_INFORMATION,
                                              security.data()) ? ERROR_SUCCESS : GetLastError();
}

// Session profiles belong to the actual desktop user. The service keeps the
// user's kernel token, and never starts an APK while impersonating that user.
class BinderProfile {
 public:
  BinderProfile(HANDLE token, std::wstring name) : token_(token), name_(std::move(name)) {}
  ~BinderProfile() {
    if (sid_.value && ImpersonateLoggedOnUser(token_)) {
      appcontainer::DeleteContainerProfile(name_);
      if (!RevertToSelf()) std::abort();
    }
  }
  DWORD Create() {
    if (!ImpersonateLoggedOnUser(token_)) return GetLastError();
    const DWORD result = appcontainer::EnsureProfile(name_, L"Gesher protected Binder", &sid_);
    if (!RevertToSelf()) std::abort();
    return result;
  }
  PSID sid() const { return sid_.value; }
 private:
  HANDLE token_;
  std::wstring name_;
  appcontainer::Sid sid_;
};

DWORD StartProtectedBinder(const std::wstring& directory, DWORD session, HANDLE stop) {
  HANDLE user = nullptr;
  if (!WTSQueryUserToken(session, &user)) return GetLastError();
  const ScopedHandle desktop_user(user);
  alignas(TOKEN_USER) BYTE identity[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
  DWORD size = 0;
  if (!GetTokenInformation(user, TokenUser, identity, sizeof(identity), &size)) return GetLastError();

  std::wstring registration, listeners, data;
  DWORD status = BinderAuthorityControl::CreateLocator(&registration);
  if (status == ERROR_SUCCESS) status = BinderAuthorityControl::CreateLocator(&listeners);
  if (status == ERROR_SUCCESS) status = BinderListenerFactory::CreateLocator(&data);
  if (status != ERROR_SUCCESS) return status;

  const std::wstring runtime = directory + L"\\runtime";
  const std::wstring binary = runtime + L"\\binderd.exe";
  appcontainer::Sid capability;
  status = appcontainer::CapabilitySid(appcontainer::kRuntimeFilesCapability, &capability);
  if (status != ERROR_SUCCESS) return status;
  constexpr DWORD kReadExecute = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
  status = appcontainer::Grant(runtime, capability.value, kReadExecute, false);
  if (status == ERROR_SUCCESS) status = appcontainer::Grant(binary, capability.value, kReadExecute, false);
  if (status != ERROR_SUCCESS) return status;

  BinderProfile profile(user, L"Gesher.ProtectedBinder." + registration.substr(registration.size() - 32));
  status = profile.Create();
  if (status != ERROR_SUCCESS) return status;
  SID_AND_ATTRIBUTES capabilities[] = {{capability.value, SE_GROUP_ENABLED}};
  const SECURITY_CAPABILITIES lowbox{profile.sid(), capabilities, 1, 0};
  std::wstring arguments = L"--gesher-protected-binder " + registration + L" " + listeners + L" " + data;

  wchar_t windows[MAX_PATH]{};
  const UINT root_length = GetWindowsDirectoryW(windows, MAX_PATH);
  if (root_length == 0 || root_length >= MAX_PATH) return ERROR_PATH_NOT_FOUND;
  const std::wstring system_root(windows, root_length);
  std::wstring environment = L"PATH=" + system_root + L"\\System32";
  environment.push_back(L'\0');
  environment += L"SystemRoot=" + system_root;
  environment.push_back(L'\0');
  environment += L"WINDIR=" + system_root;
  environment.append(2, L'\0');
  const AuthorityLaunchSpec spec{binary.c_str(), runtime.c_str(), arguments, environment, lowbox,
                                 reinterpret_cast<const TOKEN_USER*>(identity)->User.Sid, session};
  std::unique_ptr<AuthorityLaunch> broker;
  status = AuthorityLaunch::Create(user, spec, &broker);
  if (status != ERROR_SUCCESS) return status;

  // Bootstrap only the restricted Binder realm. No Android app is admitted
  // before the separate signed-enrollment and launcher paths register it.
  const PSID contained[] = {profile.sid()};
  std::unique_ptr<BinderListenerFactory> factory;
  status = BinderListenerFactory::Create(*broker, data, contained, &factory);
  if (status != ERROR_SUCCESS) return status;
  std::unique_ptr<BinderAuthorityControl> control, listener_control;
  status = BinderAuthorityControl::Connect(*broker, registration, stop, &control);
  if (status == ERROR_SUCCESS) {
    status = BinderAuthorityControl::ConnectListeners(*broker, listeners, stop, &listener_control);
  }
  if (status != ERROR_SUCCESS) return status;
  return listener_control->ServeListeners(*factory);
}

struct ServiceContext {
  SERVICE_STATUS_HANDLE status = nullptr;
  HANDLE stop = nullptr;
};

void Report(ServiceContext& service, DWORD state, DWORD error = ERROR_SUCCESS) {
  SERVICE_STATUS status{};
  status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  status.dwCurrentState = state;
  status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
  status.dwWin32ExitCode = error;
  if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
    status.dwCheckPoint = 1;
    status.dwWaitHint = 5000;
  }
  SetServiceStatus(service.status, &status);
}

DWORD WINAPI HandleServiceControl(DWORD control, DWORD, void*, void* context) {
  auto& service = *static_cast<ServiceContext*>(context);
  if (control == SERVICE_CONTROL_STOP) {
    Report(service, SERVICE_STOP_PENDING);
    SetEvent(service.stop);
    return ERROR_SUCCESS;
  }
  return control == SERVICE_CONTROL_INTERROGATE ? ERROR_SUCCESS : ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI ServiceMain(DWORD argc, wchar_t** argv) {
  ScopedHandle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  const DWORD event_error = stop.get() ? ERROR_SUCCESS : GetLastError();
  ServiceContext service{nullptr, stop.get()};
  service.status = RegisterServiceCtrlHandlerExW(kService, HandleServiceControl, &service);
  if (!service.status) return;
  Report(service, SERVICE_START_PENDING);
  DWORD desktop_session = 0;
  if (argc == 3 && std::wstring_view(argv[1]) == L"binder-session") {
    const std::wstring_view requested(argv[2]);
    if (!requested.empty() && requested.size() <= 9 &&
        requested.find_first_not_of(L"0123456789") == std::wstring_view::npos) {
      desktop_session = std::wcstoul(argv[2], nullptr, 10);
    }
  }
  const bool valid_arguments = argc == 1 || desktop_session != 0;
  std::wstring image, directory;
  DWORD status = event_error != ERROR_SUCCESS ? event_error :
      (valid_arguments && IsLocalSystem() ? TrustedImage(&image, &directory) : ERROR_ACCESS_DENIED);
  if (status == ERROR_SUCCESS) status = ValidateRuntime(directory);
  if (status == ERROR_SUCCESS) status = ClearInterruptedBootstrapQueries();
  if (status != ERROR_SUCCESS) {
    Report(service, SERVICE_STOPPED, status);
    return;
  }
  // No clients are enrolled until the protected application launch path can
  // bind the installed package and running process to a selected disk grant.
  Report(service, SERVICE_RUNNING);
  if (desktop_session != 0) {
    status = StartProtectedBinder(directory, desktop_session, stop.get());
    if (status == ERROR_OPERATION_ABORTED && WaitForSingleObject(stop.get(), 0) == WAIT_OBJECT_0) {
      status = ERROR_SUCCESS;
    }
  } else if (WaitForSingleObject(stop.get(), INFINITE) != WAIT_OBJECT_0) {
    status = GetLastError();
  }
  Report(service, SERVICE_STOPPED, status);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)) return 1;
  if (argc != 2) return 2;
  const std::wstring_view verb(argv[1]);
  if (verb == L"service") {
    wchar_t name[] = L"GesherUsbAuthority";
    SERVICE_TABLE_ENTRYW dispatch[] = {{name, ServiceMain}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(dispatch) ? 0 : 1;
  }
  if (verb != L"install-service" && verb != L"uninstall-service") return 2;
  if (!Elevated()) {
    std::fputs("The Gesher USB authority installer requires administrator elevation.\n", stderr);
    return 1;
  }
  std::wstring image;
  DWORD error = TrustedImage(&image);
  if (error == ERROR_SUCCESS) error = verb == L"install-service" ? Install(image) : Uninstall(image);
  if (error != ERROR_SUCCESS) {
    std::fprintf(stderr, "Gesher USB authority installer failed (Windows error %lu).\n",
                 static_cast<unsigned long>(error));
  }
  return error == ERROR_SUCCESS ? 0 : 1;
}
