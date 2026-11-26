// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Test-only SCM profile lifetime comparison. In particular, this unit never
// calls CreateProfile, LoadUserProfile, UnloadUserProfile or an AppContainer API.
#include "usb_scm_profile_control.h"
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <userenv.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace gesher::usb_storage::test {
namespace {
struct Close { void operator()(void* h) const { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
struct ScClose { void operator()(SC_HANDLE h) const { if (h) CloseServiceHandle(h); } };
struct Free { void operator()(void* p) const { if (p) LocalFree(p); } };
using Handle = std::unique_ptr<void, Close>;
using Service = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ScClose>;
using Local = std::unique_ptr<void, Free>;
struct Failure { DWORD error; const char* stage; };
void Need(bool ok, const char* stage, DWORD error = ERROR_SUCCESS) {
  if (!ok) { if (!error) error = GetLastError(); throw Failure{error ? error : ERROR_GEN_FAILURE, stage}; }
}
void Check(DWORD error, const char* stage) { Need(error == ERROR_SUCCESS, stage, error); }
bool Missing(DWORD error) { return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND; }
bool Same(std::wstring_view a, std::wstring_view b) {
  return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
      b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
template <size_t N> void Copy(wchar_t (&out)[N], const std::wstring& value) {
  Need(value.size() < N, "bounded-text", ERROR_INVALID_DATA);
  std::wmemcpy(out, value.c_str(), value.size() + 1);
}
template <size_t N> bool Terminated(const wchar_t (&value)[N]) { return std::wmemchr(value, 0, N) != nullptr; }
bool Suffix(std::wstring_view value) {
  size_t dash = value.find(L'-');
  return dash != std::wstring_view::npos && dash != 0 && dash + 1 < value.size() &&
      std::all_of(value.begin(), value.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || c == L'-'; }) &&
      value.find(L'-', dash + 1) == std::wstring_view::npos;
}
constexpr std::wstring_view kServicePrefix = L"GesherScmProfileControl-";
bool ServiceName(std::wstring_view value) {
  return value.size() < 80 && value.starts_with(kServicePrefix) && Suffix(value.substr(kServicePrefix.size()));
}
ULONGLONG Ticks(const FILETIME& t) { return (static_cast<ULONGLONG>(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
DWORD Remaining(ULONGLONG deadline, const char* stage) {
  const ULONGLONG now = GetTickCount64();
  Need(now < deadline, stage, ERROR_TIMEOUT);
  return static_cast<DWORD>((std::min)(deadline - now, static_cast<ULONGLONG>(15000)));
}
ULONGLONG PhaseDeadline(ULONGLONG deadline, DWORD maximum) {
  return (std::min)(deadline, GetTickCount64() + maximum);
}
void Pause(ULONGLONG deadline, const char* stage) { Sleep((std::min)(Remaining(deadline, stage), DWORD{25})); }
bool ParseDeadline(const wchar_t* text, ULONGLONG* result) {
  if (!text || text[0] < L'1' || text[0] > L'9') return false;
  ULONGLONG value = 0;
  for (size_t i = 0; i <= 20; ++i) {
    if (!text[i]) { *result = value; return true; }
    if (i == 20 || text[i] < L'0' || text[i] > L'9') return false;
    const ULONGLONG digit = static_cast<ULONGLONG>(text[i] - L'0');
    if (value > ((std::numeric_limits<ULONGLONG>::max)() - digit) / 10) return false;
    value = value * 10 + digit;
  }
  return false;
}

struct FileIdentity { DWORD volume = 0, high = 0, low = 0, size = 0; ULONGLONG written = 0; };
FileIdentity Identify(HANDLE file, bool directory = false) {
  BY_HANDLE_FILE_INFORMATION i{};
  Need(GetFileInformationByHandle(file, &i), "file-identity");
  Need(!(i.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
      !!(i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory &&
      (directory || (i.nNumberOfLinks == 1 && !i.nFileSizeHigh && i.nFileSizeLow <= 64 * 1024 * 1024)),
      "plain-owned-file", ERROR_INVALID_DATA);
  return {i.dwVolumeSerialNumber, i.nFileIndexHigh, i.nFileIndexLow, i.nFileSizeLow, Ticks(i.ftLastWriteTime)};
}
bool SameFile(const FileIdentity& a, const FileIdentity& b) {
  return a.volume == b.volume && a.high == b.high && a.low == b.low && a.size == b.size && a.written == b.written;
}
Local Descriptor(const wchar_t* sid = nullptr, bool service = false) {
  std::wstring value = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)";
  if (service) value = L"O:BAG:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)";
  if (sid) value += L"(A;;0x1200a9;;;" + std::wstring(sid) + L")";
  // Children receive explicit descriptors; the service never inherits write
  // access to the ownership manifest from the directory's read/execute ACE.
  PSECURITY_DESCRIPTOR raw = nullptr;
  Need(ConvertStringSecurityDescriptorToSecurityDescriptorW(value.c_str(), SDDL_REVISION_1, &raw, nullptr),
      "private-security-descriptor");
  return Local(raw);
}
void Sealed(HANDLE object, bool manifest) {
  PSID owner = nullptr; PACL acl = nullptr; PSECURITY_DESCRIPTOR raw = nullptr;
  Check(GetSecurityInfo(object, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      &owner, nullptr, &acl, nullptr, &raw), "read-file-security");
  const Local descriptor(raw);
  SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0;
  Need(owner && (IsWellKnownSid(owner, WinBuiltinAdministratorsSid) || IsWellKnownSid(owner, WinLocalSystemSid)) &&
      acl && GetSecurityDescriptorControl(raw, &control, &revision) && (control & SE_DACL_PROTECTED),
      "protected-file-owner-dacl", ERROR_ACCESS_DENIED);
  for (DWORD n = 0; n < acl->AceCount; ++n) {
    void* raw_ace = nullptr; Need(GetAce(acl, n, &raw_ace), "read-file-ace");
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
    Need(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && !(ace->Header.AceFlags & INHERIT_ONLY_ACE),
        "simple-file-ace", ERROR_ACCESS_DENIED);
    PSID sid = const_cast<DWORD*>(&ace->SidStart);
    if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) continue;
    DWORD access = ace->Mask;
    GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    MapGenericMask(&access, &mapping);
    Need(!manifest && !(access & ~(FILE_GENERIC_READ | FILE_GENERIC_EXECUTE)), "no-untrusted-file-write", ERROR_ACCESS_DENIED);
  }
}
Handle OpenFile(const std::wstring& path, DWORD access, bool directory = false) {
  Handle file(CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
  Need(file.get() != INVALID_HANDLE_VALUE, "open-owned-file");
  Identify(file.get(), directory);
  return file;
}
std::wstring Image() {
  std::array<wchar_t, 32768> text{};
  DWORD n = GetModuleFileNameW(nullptr, text.data(), static_cast<DWORD>(text.size()));
  Need(n && n < text.size(), "control-image-path");
  return {text.data(), n};
}
struct Paths { std::wstring root, image, manifest, next; };
Paths Locations(const std::wstring& image, const std::wstring& directory) {
  PWSTR raw = nullptr;
  const HRESULT result = SHGetKnownFolderPath(FOLDERID_ProgramFilesX64, 0, nullptr, &raw);
  Need(result == S_OK && raw, "program-files-root", HRESULT_CODE(result));
  const std::wstring prefix = std::wstring(raw) + L"\\Gesher protected-launch-test-";
  CoTaskMemFree(raw);
  Need(directory.size() < 240 && directory.size() > prefix.size() &&
      Same(std::wstring_view(directory).substr(0, prefix.size()), prefix) &&
      Suffix(std::wstring_view(directory).substr(prefix.size())) && Same(image, directory + L"\\probe.exe"),
      "exact-primary-fixture-locator", ERROR_INVALID_NAME);
  const std::wstring root = directory + L"-scm-control";
  return {root, root + L"\\control.exe", root + L"\\ownership.bin", root + L"\\ownership.next"};
}
std::wstring Account(const wchar_t* name) { return L"NT SERVICE\\" + std::wstring(name); }
std::wstring Command(const Paths& paths, const wchar_t* name) {
  return L"\"" + paths.image + L"\" --scm-profile-control-service " + name;
}
std::wstring SidText(PSID sid) {
  const SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
  Need(IsValidSid(sid) && *GetSidSubAuthorityCount(sid) == 6 &&
      std::memcmp(GetSidIdentifierAuthority(sid), &nt, sizeof(nt)) == 0 && *GetSidSubAuthority(sid, 0) == 80,
      "virtual-service-sid", ERROR_INVALID_SID);
  LPWSTR raw = nullptr; Need(ConvertSidToStringSidW(sid, &raw), "service-sid-text");
  const Local text(raw); return raw;
}
std::wstring AccountSid(const wchar_t* name) {
  alignas(SID) BYTE sid[SECURITY_MAX_SID_SIZE]{};
  wchar_t domain[256]{}; DWORD bytes = sizeof(sid), chars = static_cast<DWORD>(std::size(domain)); SID_NAME_USE use{};
  Need(LookupAccountNameW(nullptr, Account(name).c_str(), sid, &bytes, domain, &chars, &use), "lookup-control-account");
  return SidText(sid);
}
bool CanonicalSid(const wchar_t* text) {
  PSID raw = nullptr;
  if (!ConvertStringSidToSidW(text, &raw)) return false;
  const Local sid(raw);
  try { return SidText(raw) == text; } catch (...) { return false; }
}
DWORD ProfileKey(const wchar_t* sid, bool hive) {
  const std::wstring key = hive ? sid : L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\" + std::wstring(sid);
  HKEY opened = nullptr;
  const LSTATUS result = RegOpenKeyExW(hive ? HKEY_USERS : HKEY_LOCAL_MACHINE, key.c_str(), 0,
      KEY_READ | (hive ? 0 : KEY_WOW64_64KEY), &opened);
  if (opened) RegCloseKey(opened);
  return static_cast<DWORD>(result);
}
bool ProcessUser(HANDLE process, const wchar_t* expected = nullptr) {
  HANDLE raw = nullptr; Need(OpenProcessToken(process, TOKEN_QUERY, &raw), "query-control-process-token");
  const Handle token(raw);
  alignas(TOKEN_USER) BYTE bytes[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{}; DWORD count = 0;
  Need(GetTokenInformation(token.get(), TokenUser, bytes, sizeof(bytes), &count), "read-control-token-user");
  PSID sid = reinterpret_cast<TOKEN_USER*>(bytes)->User.Sid;
  return expected ? SidText(sid) == expected : IsWellKnownSid(sid, WinLocalSystemSid) != FALSE;
}

enum : DWORD { kCreated = 1, kAbsent = 2, kStartRequested = 4, kVerified = 8,
  kExited = 16, kServiceDeleted = 32, kHandlesClosed = 64, kDeleteFinished = 128 };
struct Manifest {
  DWORD magic = 0x47534350, version = 1, flags = 0, reserved = 0;
  FileIdentity source, image;
  ScmProfileControlEvidence observed;
};
static_assert(std::is_trivially_copyable_v<Manifest> && sizeof(Manifest) <= 2048);
void WriteManifest(const Paths& paths, const Manifest& value, bool first = false) {
  const Local descriptor = Descriptor(); SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.get(), FALSE};
  const std::wstring& target = first ? paths.manifest : paths.next;
  Handle file(CreateFileW(target.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_NEW,
      FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  Need(file.get() != INVALID_HANDLE_VALUE, "exclusive-manifest-write");
  DWORD count = 0;
  Need(WriteFile(file.get(), &value, sizeof(value), &count, nullptr) && count == sizeof(value) && FlushFileBuffers(file.get()),
      "durable-manifest-write");
  file.reset();
  if (!first) Need(MoveFileExW(paths.next.c_str(), paths.manifest.c_str(),
      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "atomic-manifest-replace");
}
Manifest ReadManifest(const Paths& paths, const std::wstring& source) {
  const Handle root = OpenFile(paths.root, FILE_READ_ATTRIBUTES | READ_CONTROL, true); Sealed(root.get(), false);
  const Handle file = OpenFile(paths.manifest, GENERIC_READ | READ_CONTROL); Sealed(file.get(), true);
  Need(Identify(file.get()).size == sizeof(Manifest), "complete-manifest-size", ERROR_INVALID_DATA);
  Manifest m{}; DWORD count = 0;
  Need(ReadFile(file.get(), &m, sizeof(m), &count, nullptr) && count == sizeof(m), "read-manifest");
  Need(m.magic == 0x47534350 && m.version == 1 && !m.reserved && !(m.flags & ~255u) &&
      m.observed.version == 1 && Terminated(m.observed.service_name) && ServiceName(m.observed.service_name) &&
      Terminated(m.observed.sid) && (!m.observed.sid[0] || CanonicalSid(m.observed.sid)) &&
      std::memchr(m.observed.stage, 0, sizeof(m.observed.stage)) &&
      std::memchr(m.observed.cleanup_stage, 0, sizeof(m.observed.cleanup_stage)), "valid-ownership-manifest", ERROR_INVALID_DATA);
  const Handle original = OpenFile(source, GENERIC_READ | READ_CONTROL); Sealed(original.get(), false);
  const Handle image = OpenFile(paths.image, GENERIC_READ | READ_CONTROL); Sealed(image.get(), false);
  Need(SameFile(m.source, Identify(original.get())) && SameFile(m.image, Identify(image.get())),
      "manifest-exact-source-and-copy", ERROR_ACCESS_DENIED);
  Need(!(m.flags & kStartRequested) || ((m.flags & (kCreated | kAbsent)) == (kCreated | kAbsent) && m.observed.sid[0]),
      "manifest-start-ownership", ERROR_INVALID_DATA);
  Need(!(m.flags & kVerified) || ((m.flags & kStartRequested) && m.observed.pid && m.observed.process_creation &&
      m.observed.identity_verified == 1 && m.observed.image_verified == 1), "manifest-process-proof", ERROR_INVALID_DATA);
  Need(!(m.flags & kExited) || ((m.flags & kVerified) && m.observed.retained_process_exit == 1),
      "manifest-exit-proof", ERROR_INVALID_DATA);
  Need(!(m.flags & kHandlesClosed) || ((m.flags & (kExited | kServiceDeleted)) == (kExited | kServiceDeleted) &&
      m.observed.handles_closed == 1), "manifest-handle-retirement", ERROR_INVALID_DATA);
  Need(!(m.flags & kDeleteFinished) || ((m.flags & kHandlesClosed) && m.observed.delete_attempts &&
      (m.observed.outcome == ScmProfileControlOutcome::inconclusive ||
       (m.observed.profile_while_running == ERROR_SUCCESS && m.observed.hive_while_running == ERROR_SUCCESS))),
      "manifest-profile-observation", ERROR_INVALID_DATA);
  // A deleted virtual account need not remain name-resolvable. Its protected
  // record binds the SID to the name, exclusive creation and observed TokenUser.
  // While a service still exists, also demand a fresh account/SID translation.
  if (m.observed.sid[0] && !(m.flags & kServiceDeleted))
    Need(AccountSid(m.observed.service_name) == m.observed.sid, "manifest-account-sid-binding", ERROR_INVALID_SID);
  return m;
}
void Save(Manifest* m, const Paths& paths, const ScmProfileControlEvidence& evidence) {
  m->observed = evidence; WriteManifest(paths, *m);
}
void RefreshObservation(const Manifest& m, ScmProfileControlEvidence* evidence) {
  const auto prior = *evidence;
  if (prior.delete_attempts) {
    Need((m.flags & kDeleteFinished) && m.observed.outcome == prior.outcome &&
        m.observed.delete_status == prior.delete_status, "uncommitted-profile-deletion-result", ERROR_INVALID_DATA);
  }
  *evidence = m.observed;
  if (prior.attempted) {
    evidence->elapsed_ms = prior.elapsed_ms;
    evidence->remaining_worker_ms = prior.remaining_worker_ms;
    if (prior.outcome == ScmProfileControlOutcome::inconclusive && prior.status != ERROR_NOT_READY) {
      // Resource cleanup may still proceed, but cannot erase a failed original
      // observation and turn an incomplete comparison into a successful one.
      evidence->status = prior.status;
      std::memcpy(evidence->stage, prior.stage, sizeof(prior.stage));
      evidence->outcome = ScmProfileControlOutcome::inconclusive;
    }
  }
  evidence->cleanup_attempted = 1;
}
void AllowControlRead(const std::wstring& path, const wchar_t* sid, bool directory) {
  const Handle file = OpenFile(path, READ_CONTROL | WRITE_DAC, directory);
  const Local descriptor = Descriptor(sid); PACL acl = nullptr; BOOL present = FALSE, defaulted = FALSE;
  Need(GetSecurityDescriptorDacl(descriptor.get(), &present, &acl, &defaulted) && present && acl, "control-read-dacl");
  Check(SetSecurityInfo(file.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
      nullptr, nullptr, acl, nullptr), "grant-only-control-read-execute");
}
void CopyImage(const std::wstring& source, const Paths& paths, Manifest* m, ULONGLONG deadline) {
  const Handle input = OpenFile(source, GENERIC_READ | READ_CONTROL); Sealed(input.get(), false);
  m->source = Identify(input.get()); Need(m->source.size != 0, "nonempty-source-image", ERROR_INVALID_DATA);
  const Local descriptor = Descriptor(); SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.get(), FALSE};
  Handle output(CreateFileW(paths.image.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ, &attributes, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  Need(output.get() != INVALID_HANDLE_VALUE, "exclusive-control-image");
  std::array<BYTE, 65536> buffer{}; DWORD total = 0;
  while (total < m->source.size) {
    Remaining(deadline, "copy-image-budget");
    DWORD read = 0, written = 0;
    Need(ReadFile(input.get(), buffer.data(), (std::min)(static_cast<DWORD>(buffer.size()), m->source.size - total),
        &read, nullptr) && read, "read-control-image-bytes");
    Need(WriteFile(output.get(), buffer.data(), read, &written, nullptr) && written == read, "copy-control-image-bytes");
    total += read;
  }
  Need(FlushFileBuffers(output.get()), "flush-control-image");
  output.reset();
  // Windows finalizes last-write metadata when the last write handle closes.
  const Handle completed = OpenFile(paths.image, FILE_READ_ATTRIBUTES | READ_CONTROL);
  m->image = Identify(completed.get());
  Need(m->image.size == m->source.size && SameFile(m->source, Identify(input.get())), "stable-source-copy", ERROR_INVALID_DATA);
}
SERVICE_STATUS_PROCESS State(SC_HANDLE service) {
  SERVICE_STATUS_PROCESS value{}; DWORD bytes = 0;
  Need(QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&value), sizeof(value), &bytes),
      "query-control-service");
  return value;
}
void ValidateService(SC_HANDLE service, const Paths& paths, const Manifest& m) {
  // QueryServiceConfigW's RPC contract caps the advertised buffer at 8 KiB.
  alignas(QUERY_SERVICE_CONFIGW) BYTE bytes[8192]{}; DWORD size = 0;
  auto* c = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(bytes);
  Need(QueryServiceConfigW(service, c, sizeof(bytes), &size), "query-control-config");
  Need(c->lpBinaryPathName && Same(c->lpBinaryPathName, Command(paths, m.observed.service_name)) &&
      c->lpServiceStartName && Same(c->lpServiceStartName, Account(m.observed.service_name)) &&
      c->dwServiceType == SERVICE_WIN32_OWN_PROCESS && c->dwStartType == SERVICE_DEMAND_START,
      "exact-control-config", ERROR_ACCESS_DENIED);
  Need(QueryServiceObjectSecurity(service, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      bytes, sizeof(bytes), &size), "query-control-service-security");
  PSID owner = nullptr; PACL acl = nullptr; BOOL present = FALSE, defaulted = FALSE;
  Need(GetSecurityDescriptorOwner(bytes, &owner, &defaulted) && owner &&
      (IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid)) &&
      GetSecurityDescriptorDacl(bytes, &present, &acl, &defaulted) && present && acl,
      "control-service-owner-dacl", ERROR_ACCESS_DENIED);
  for (DWORD i = 0; i < acl->AceCount; ++i) {
    void* raw = nullptr; Need(GetAce(acl, i, &raw), "control-service-ace");
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
    Need(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && !ace->Header.AceFlags &&
        (IsWellKnownSid(const_cast<DWORD*>(&ace->SidStart), WinLocalSystemSid) ||
         IsWellKnownSid(const_cast<DWORD*>(&ace->SidStart), WinBuiltinAdministratorsSid)),
        "only-system-admin-controls-service", ERROR_ACCESS_DENIED);
  }
}
ULONGLONG VerifyProcess(HANDLE process, const Paths& paths, const Manifest& m) {
  std::array<wchar_t, 32768> name{}; DWORD capacity = static_cast<DWORD>(name.size());
  Need(QueryFullProcessImageNameW(process, 0, name.data(), &capacity) && Same({name.data(), capacity}, paths.image),
      "exact-control-process-image", ERROR_ACCESS_DENIED);
  const Handle image = OpenFile(paths.image, GENERIC_READ | READ_CONTROL); Sealed(image.get(), false);
  Need(SameFile(m.image, Identify(image.get())), "exact-control-image-object", ERROR_ACCESS_DENIED);
  FILETIME created{}, exited{}, kernel{}, user{};
  Need(GetProcessTimes(process, &created, &exited, &kernel, &user) && Ticks(created), "exact-control-process-creation");
  return Ticks(created);
}
void StopRetained(Service* service, Handle* process, Manifest* m, const Paths& paths,
    ULONGLONG deadline, ScmProfileControlEvidence* e) {
  Remaining(deadline, "stop-control-budget");
  const auto before = State(service->get());
  if (before.dwCurrentState != SERVICE_STOPPED) {
    Need(before.dwProcessId == e->pid, "stop-exact-control-pid", ERROR_ACCESS_DENIED);
    SERVICE_STATUS ignored{};
    if (!ControlService(service->get(), SERVICE_CONTROL_STOP, &ignored)) {
      const DWORD error = GetLastError(); Need(error == ERROR_SERVICE_NOT_ACTIVE, "stop-control-service", error);
    }
  }
  const ULONGLONG stop_deadline = PhaseDeadline(deadline, 5000);
  const DWORD wait = WaitForSingleObject(process->get(), Remaining(stop_deadline, "control-exit-budget"));
  Need(wait == WAIT_OBJECT_0, "retained-control-process-exit",
      wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : wait == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA);
  e->retained_process_exit = 1; m->flags |= kExited; Save(m, paths, *e);
  while (State(service->get()).dwCurrentState != SERVICE_STOPPED) Pause(stop_deadline, "control-stopped-budget");
  Need(DeleteService(service->get()), "delete-owned-control-service");
  e->service_deleted = 1; m->flags |= kServiceDeleted;
  service->reset(); process->reset();
  // All token and hive handles were scoped to their queries. The retained
  // process and last service handles are closed before the real deletion API.
  e->handles_closed = 1; m->flags |= kHandlesClosed; Save(m, paths, *e);
}
void DeleteObservedProfile(Manifest* m, const Paths& paths, ULONGLONG deadline, ScmProfileControlEvidence* e) {
  Need((m->flags & (kAbsent | kVerified | kExited | kServiceDeleted | kHandlesClosed)) ==
      (kAbsent | kVerified | kExited | kServiceDeleted | kHandlesClosed), "profile-deletion-lifetime-proof", ERROR_INVALID_DATA);
  const bool observed = e->profile_while_running == ERROR_SUCCESS && e->hive_while_running == ERROR_SUCCESS;
  const ULONGLONG deletion_deadline = PhaseDeadline(deadline, 5000);
  do {
    Remaining(deletion_deadline, "control-profile-delete-budget");
    ++e->delete_attempts;
    const BOOL deleted = DeleteProfileW(e->sid, nullptr, nullptr);
    e->delete_status = deleted ? ERROR_SUCCESS : GetLastError();
    if (!deleted && !e->delete_status) e->delete_status = ERROR_GEN_FAILURE;
    if (deleted || (e->delete_status != ERROR_SHARING_VIOLATION && e->delete_status != ERROR_LOCK_VIOLATION)) break;
    if (e->delete_attempts >= 21 || GetTickCount64() >= deletion_deadline) break;
    Sleep((std::min)(Remaining(deletion_deadline, "control-profile-delete-budget"), DWORD{250}));
  } while (GetTickCount64() < deletion_deadline);
  e->profile_after_delete = ProfileKey(e->sid, false); e->hive_after_delete = ProfileKey(e->sid, true);
  e->outcome = !observed ? ScmProfileControlOutcome::inconclusive : e->delete_status == ERROR_SUCCESS ?
      ScmProfileControlOutcome::delete_succeeded : ScmProfileControlOutcome::delete_failed;
  e->status = e->delete_status;
  std::snprintf(e->stage, sizeof(e->stage), "%s", observed ?
      "actual-delete-profile-result" : "no-complete-running-profile-observation");
  m->flags |= kDeleteFinished; Save(m, paths, *e);
}

struct ControlState { SERVICE_STATUS_HANDLE status = nullptr; HANDLE stop = nullptr; };
std::wstring service_name;
DWORD service_result = ERROR_NOT_READY;
void Report(ControlState& state, DWORD value, DWORD error = ERROR_SUCCESS) {
  SERVICE_STATUS status{}; status.dwServiceType = SERVICE_WIN32_OWN_PROCESS; status.dwCurrentState = value;
  status.dwControlsAccepted = value == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0; status.dwWin32ExitCode = error;
  SetServiceStatus(state.status, &status);
}
DWORD WINAPI ServiceControl(DWORD operation, DWORD, void*, void* context) {
  if (operation == SERVICE_CONTROL_STOP) { SetEvent(static_cast<ControlState*>(context)->stop); return ERROR_SUCCESS; }
  return operation == SERVICE_CONTROL_INTERROGATE ? ERROR_SUCCESS : ERROR_CALL_NOT_IMPLEMENTED;
}
void WINAPI ServiceMain(DWORD argc, wchar_t** argv) {
  ControlState state;
  const Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr)); state.stop = stop.get();
  state.status = RegisterServiceCtrlHandlerExW(service_name.c_str(), ServiceControl, &state);
  if (!state.status) { service_result = GetLastError(); return; }
  try {
    Need(!!stop, "control-stop-event");
    // Identity is independently verified through the retained process by the
    // SYSTEM observer. This service itself is only an SCM stop-event control.
    ULONGLONG deadline = 0;
    const ULONGLONG now = GetTickCount64();
    Need(argc == 2 && argv && Same(argv[0], service_name) && ParseDeadline(argv[1], &deadline) &&
        deadline > now && deadline - now <= 15000,
        "control-service-existing-deadline", ERROR_INVALID_PARAMETER);
    Report(state, SERVICE_RUNNING);
    // StartService supplies the SAME absolute bounded observation deadline as
    // its one service argument. An abandoned control cannot outlive that bound.
    const DWORD wait = WaitForSingleObject(stop.get(), Remaining(deadline, "control-service-stop-budget"));
    service_result = wait == WAIT_OBJECT_0 ? ERROR_SUCCESS : wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
  } catch (const Failure& failure) { service_result = failure.error; }
  catch (...) { service_result = ERROR_NOT_ENOUGH_MEMORY; }
  Report(state, SERVICE_STOPPED, service_result);
}
}  // namespace

bool DispatchScmProfileControlService(int argc, wchar_t** argv, DWORD* exit_code) noexcept {
  if (argc < 2 || std::wstring_view(argv[1]) != L"--scm-profile-control-service") return false;
  if (!exit_code) return true;
  *exit_code = ERROR_INVALID_PARAMETER;
  try {
    if (argc != 3 || !ServiceName(argv[2])) return true;
    service_name = argv[2];
    SERVICE_TABLE_ENTRYW entries[] = {{service_name.data(), ServiceMain}, {nullptr, nullptr}};
    *exit_code = StartServiceCtrlDispatcherW(entries) ? service_result : GetLastError();
  } catch (...) { *exit_code = ERROR_NOT_ENOUGH_MEMORY; }
  return true;
}

void ObserveScmProfileControl(const std::wstring& sealed_image, const std::wstring& sealed_directory,
    ULONGLONG worker_deadline, ScmProfileControlEvidence* evidence) noexcept {
  if (!evidence) return;
  *evidence = {}; auto& e = *evidence; e.attempted = 1; e.outcome = ScmProfileControlOutcome::inconclusive;
  const ULONGLONG started = GetTickCount64();
  const ULONGLONG deadline = (std::min)(worker_deadline, started + 15000);
  bool manifest_written = false;
  try {
    Need(ProcessUser(GetCurrentProcess()), "control-requires-system-worker", ERROR_ACCESS_DENIED);
    HANDLE raw_thread = nullptr;
    const BOOL has_thread_token = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw_thread);
    const DWORD thread_error = GetLastError(); const Handle thread_token(raw_thread);
    Need(!has_thread_token && thread_error == ERROR_NO_TOKEN, "control-requires-unimpersonated-worker", ERROR_BAD_IMPERSONATION_LEVEL);
    Need(Same(Image(), sealed_image), "control-actual-sealed-worker-image", ERROR_ACCESS_DENIED);
    Remaining(deadline, "control-start-budget");
    const Paths paths = Locations(sealed_image, sealed_directory);
    // Verify the supplied parent is sealed, but never modify it or its image.
    { const Handle parent = OpenFile(sealed_directory, FILE_READ_ATTRIBUTES | READ_CONTROL, true); Sealed(parent.get(), false); }
    Manifest m;
    Copy(e.service_name, std::wstring(kServicePrefix) + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(started));
    const Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    Need(!!manager, "control-open-scm");
    { const Service prior(OpenServiceW(manager.get(), e.service_name, SERVICE_QUERY_STATUS));
      const DWORD error = GetLastError(); Need(!prior && error == ERROR_SERVICE_DOES_NOT_EXIST,
          "refuse-preexisting-control-service", prior ? ERROR_ALREADY_EXISTS : error); }
    const Local descriptor = Descriptor(); SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.get(), FALSE};
    Need(CreateDirectoryW(paths.root.c_str(), &attributes), "exclusive-control-sibling");
    e.artifacts_retained = 1;
    CopyImage(sealed_image, paths, &m, deadline); m.observed = e; WriteManifest(paths, m, true); manifest_written = true;
    Remaining(deadline, "control-create-budget");
    Service service(CreateServiceW(manager.get(), e.service_name, e.service_name, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, Command(paths, e.service_name).c_str(),
        nullptr, nullptr, nullptr, Account(e.service_name).c_str(), nullptr));
    Need(!!service, "create-unique-virtual-control-service"); m.flags |= kCreated; Save(&m, paths, e);
    const Local service_descriptor = Descriptor(nullptr, true);
    Need(SetServiceObjectSecurity(service.get(), OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, service_descriptor.get()),
        "seal-control-service");
    Copy(e.sid, AccountSid(e.service_name));
    const DWORD profile = ProfileKey(e.sid, false), hive = ProfileKey(e.sid, true);
    Need(Missing(profile) && Missing(hive), "refuse-preexisting-control-profile-or-hive",
        !Missing(profile) ? (profile ? profile : ERROR_ALREADY_EXISTS) : (hive ? hive : ERROR_ALREADY_EXISTS));
    m.flags |= kAbsent; Save(&m, paths, e);
    AllowControlRead(paths.root, e.sid, true); AllowControlRead(paths.image, e.sid, false);
    ValidateService(service.get(), paths, m);
    m.flags |= kStartRequested; Save(&m, paths, e);
    Remaining(deadline, "control-start-service-budget");
    const std::wstring deadline_text = std::to_wstring(deadline);
    const wchar_t* arguments[] = {deadline_text.c_str()};
    Need(StartServiceW(service.get(), 1, arguments), "start-control-service");
    const ULONGLONG start_deadline = PhaseDeadline(deadline, 5000);
    SERVICE_STATUS_PROCESS state{};
    for (;;) {
      state = State(service.get()); if (state.dwCurrentState == SERVICE_RUNNING && state.dwProcessId) break;
      Need(state.dwCurrentState != SERVICE_STOPPED, "control-stopped-before-observation",
          state.dwWin32ExitCode ? state.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE);
      Pause(start_deadline, "control-running-budget");
    }
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, state.dwProcessId));
    Need(!!process, "retain-control-process");
    e.pid = state.dwProcessId; e.process_creation = VerifyProcess(process.get(), paths, m); e.image_verified = 1;
    Need(ProcessUser(process.get(), e.sid), "exact-control-process-token-user", ERROR_ACCESS_DENIED);
    const auto again = State(service.get());
    Need(again.dwProcessId == e.pid && again.dwCurrentState == SERVICE_RUNNING &&
        WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT, "running-exact-control-process", ERROR_PROCESS_ABORTED);
    e.identity_verified = 1; m.flags |= kVerified;
    e.profile_while_running = ProfileKey(e.sid, false); e.hive_while_running = ProfileKey(e.sid, true); Save(&m, paths, e);
    StopRetained(&service, &process, &m, paths, deadline, &e);
    // The manager handle is not a service/account handle; close it as well by
    // ending its scope before the deletion helper in cleanup below.
  } catch (const Failure& failure) { e.status = failure.error; std::snprintf(e.stage, sizeof(e.stage), "%s", failure.stage); }
  catch (...) { e.status = ERROR_NOT_ENOUGH_MEMORY; std::snprintf(e.stage, sizeof(e.stage), "%s", "control-allocation"); }
  if (manifest_written) CleanupScmProfileControl(sealed_image, sealed_directory, deadline, &e);
  const ULONGLONG ended = GetTickCount64();
  e.elapsed_ms = ended - started;
  e.remaining_worker_ms = ended < worker_deadline ? worker_deadline - ended : 0;
}

void CleanupScmProfileControl(const std::wstring& sealed_image, const std::wstring& sealed_directory,
    ULONGLONG existing_cleanup_deadline, ScmProfileControlEvidence* evidence) noexcept {
  if (!evidence) return;
  auto& e = *evidence; e.cleanup_attempted = 1;
  try {
    const Paths paths = Locations(sealed_image, sealed_directory);
    const DWORD attributes = GetFileAttributesW(paths.root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD error = GetLastError(); Need(Missing(error) && !e.artifacts_retained, "control-root-unavailable", error);
      e.cleanup_status = ERROR_SUCCESS; std::snprintf(e.cleanup_stage, sizeof(e.cleanup_stage), "%s", "no-control-artifacts"); return;
    }
    Remaining(existing_cleanup_deadline, "control-cleanup-budget");
    Need(!e.attempted || e.artifacts_retained, "refuse-unowned-control-sibling", ERROR_ALREADY_EXISTS);
    Manifest m = ReadManifest(paths, sealed_image);
    Need(!e.service_name[0] || (Terminated(e.service_name) && Same(e.service_name, m.observed.service_name)),
        "same-control-evidence", ERROR_INVALID_DATA);
    RefreshObservation(m, &e);
    // The manifest contains only observations already made by the SYSTEM
    // worker. An absent PID or stopped SCM entry cannot replace retained exit.
    if (!(m.flags & kServiceDeleted)) {
      Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)); Need(!!manager, "cleanup-open-scm");
      Service service(OpenServiceW(manager.get(), m.observed.service_name,
          SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE | READ_CONTROL));
      const DWORD error = GetLastError();
      if (!(m.flags & kCreated)) {
        Need(!service && error == ERROR_SERVICE_DOES_NOT_EXIST, "unconfirmed-service-creation", ERROR_INVALID_DATA);
      } else {
        Need(!!service, "cleanup-owned-service-present", error); ValidateService(service.get(), paths, m);
        if (m.flags & kStartRequested) {
          if (!(m.flags & kVerified)) {
            // An exclusively owned SCM entry can be asked to stop, but an
            // unverified/missing process can never authorize profile deletion.
            SERVICE_STATUS ignored{};
            ControlService(service.get(), SERVICE_CONTROL_STOP, &ignored);
          }
          Need((m.flags & kVerified) && m.observed.pid && m.observed.process_creation,
              "missing-exact-control-lifetime-proof", ERROR_INVALID_DATA);
          Handle process;
          if (!(m.flags & kExited)) {
            process.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, e.pid));
            Need(!!process, "cleanup-retain-exact-control-process");
            Need(VerifyProcess(process.get(), paths, m) == e.process_creation, "control-pid-not-reused", ERROR_ACCESS_DENIED);
            StopRetained(&service, &process, &m, paths, existing_cleanup_deadline, &e);
          } else {
            Need(State(service.get()).dwCurrentState == SERVICE_STOPPED, "exited-control-still-running", ERROR_INVALID_DATA);
            Need(DeleteService(service.get()), "cleanup-delete-owned-service"); service.reset();
            e.service_deleted = 1; e.handles_closed = 1; m.flags |= kServiceDeleted | kHandlesClosed; Save(&m, paths, e);
          }
        } else {
          Need(State(service.get()).dwCurrentState == SERVICE_STOPPED, "unstarted-control-is-running", ERROR_INVALID_DATA);
          Need(DeleteService(service.get()), "delete-unstarted-control-service"); service.reset();
          e.service_deleted = 1; m.flags |= kServiceDeleted; Save(&m, paths, e);
        }
      }
    }
    // All SCM/process/token/hive handles in the preceding scopes are closed.
    if (m.flags & kStartRequested) {
      if (!(m.flags & kDeleteFinished)) {
        Need(e.status == ERROR_NOT_READY, "prior-control-observation-incomplete", e.status ? e.status : ERROR_INVALID_DATA);
        DeleteObservedProfile(&m, paths, existing_cleanup_deadline, &e);
      }
      // A not-found API result plus observed absence allows owned file cleanup,
      // but is never relabeled as a demonstrated profile unload/deletion.
      const bool gone = e.delete_status == ERROR_SUCCESS || Missing(e.delete_status) || e.delete_status == ERROR_NOT_FOUND;
      Need(gone && Missing(e.profile_after_delete) && Missing(e.hive_after_delete),
          "retain-control-profile-after-delete", e.delete_status ? e.delete_status : ERROR_BUSY);
    }
    Remaining(existing_cleanup_deadline, "control-artifact-cleanup-budget");
    Need(GetFileAttributesW(paths.next.c_str()) == INVALID_FILE_ATTRIBUTES && Missing(GetLastError()),
        "retain-incomplete-manifest-update", ERROR_INVALID_DATA);
    Need(DeleteFileW(paths.image.c_str()), "delete-owned-control-image");
    Need(DeleteFileW(paths.manifest.c_str()), "delete-owned-control-manifest");
    Need(RemoveDirectoryW(paths.root.c_str()), "delete-owned-control-sibling");
    e.artifacts_retained = 0; e.cleanup_status = ERROR_SUCCESS;
    std::snprintf(e.cleanup_stage, sizeof(e.cleanup_stage), "%s", "complete");
  } catch (const Failure& failure) {
    e.cleanup_status = failure.error; e.artifacts_retained = 1;
    std::snprintf(e.cleanup_stage, sizeof(e.cleanup_stage), "%s", failure.stage);
    if (e.outcome == ScmProfileControlOutcome::inconclusive && e.status == ERROR_NOT_READY) {
      e.status = failure.error; std::snprintf(e.stage, sizeof(e.stage), "%s", failure.stage);
    }
  } catch (...) {
    e.cleanup_status = ERROR_NOT_ENOUGH_MEMORY; e.artifacts_retained = 1;
    std::snprintf(e.cleanup_stage, sizeof(e.cleanup_stage), "%s", "cleanup-allocation");
  }
  e.cleanup_attempted = 1;
}

void PrintScmProfileControlEvidence(const ScmProfileControlEvidence& e) noexcept {
  if (e.version != 1 || !Terminated(e.service_name) || !Terminated(e.sid) ||
      (e.service_name[0] && !ServiceName(e.service_name)) || (e.sid[0] && !CanonicalSid(e.sid)) ||
      static_cast<DWORD>(e.outcome) > static_cast<DWORD>(ScmProfileControlOutcome::delete_failed) ||
      !std::memchr(e.stage, 0, sizeof(e.stage)) || !std::memchr(e.cleanup_stage, 0, sizeof(e.cleanup_stage))) {
    std::puts("Protected SCM control: invalid-evidence; comparison inconclusive"); return;
  }
  const char* outcomes[] = {"not-attempted", "inconclusive", "delete-succeeded", "delete-failed"};
  std::printf("Protected SCM control: attempted=%lu outcome=%s stage=%s win32=%lu service=%ls sid=%ls\n",
      static_cast<unsigned long>(e.attempted), outcomes[static_cast<DWORD>(e.outcome)], e.stage,
      static_cast<unsigned long>(e.status), e.service_name, e.sid);
  std::printf("Protected SCM control: identity=%lu image=%lu pid=%lu created=%llu profile-running=%lu hive-running=%lu retained-exit=%lu service-deleted=%lu handles-closed=%lu\n",
      static_cast<unsigned long>(e.identity_verified), static_cast<unsigned long>(e.image_verified),
      static_cast<unsigned long>(e.pid), static_cast<unsigned long long>(e.process_creation),
      static_cast<unsigned long>(e.profile_while_running), static_cast<unsigned long>(e.hive_while_running),
      static_cast<unsigned long>(e.retained_process_exit), static_cast<unsigned long>(e.service_deleted), static_cast<unsigned long>(e.handles_closed));
  std::printf("Protected SCM control: delete-attempts=%lu delete-api=%lu profile-after=%lu hive-after=%lu cleanup-stage=%s cleanup-win32=%lu artifacts-retained=%lu elapsed-ms=%llu remaining-worker-ms=%llu\n",
      static_cast<unsigned long>(e.delete_attempts), static_cast<unsigned long>(e.delete_status),
      static_cast<unsigned long>(e.profile_after_delete), static_cast<unsigned long>(e.hive_after_delete),
      e.cleanup_stage, static_cast<unsigned long>(e.cleanup_status), static_cast<unsigned long>(e.artifacts_retained),
      static_cast<unsigned long long>(e.elapsed_ms), static_cast<unsigned long long>(e.remaining_worker_ms));
}
}  // namespace gesher::usb_storage::test
