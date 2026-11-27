// Gesher: experimental per-package AppContainer isolation (see app_container.h).
// The AppContainer APIs are declared for Windows 8 and later; standalone builds
// (tools/test_binder_lifetime.py) may not define a version.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "app_container.h"

#include <aclapi.h>
#include <objbase.h>
#include <sddl.h>
#include <userenv.h>

#include <cstring>
#include <algorithm>
#include <mutex>

namespace gesher {
namespace appcontainer {
namespace {

// DACL updates read, merge and write the whole ACL. Keep concurrent package
// reconciliations from losing one another's entries inside the broker.
std::mutex g_acl_lock;

// The AppContainer APIs, resolved at run time from System32's userenv.dll and
// kernelbase.dll: linking userenv or mincore would change every importer's
// Windows import set (binderd's is checked), for functions only the sandbox uses.
template <typename F>
F Resolve(const wchar_t* module, const char* name) {
  HMODULE handle = GetModuleHandleW(module);
  if (handle == nullptr) handle = LoadLibraryExW(module, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (handle == nullptr) return nullptr;
  return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(handle, name)));
}

using CreateProfileFn = HRESULT(WINAPI*)(PCWSTR, PCWSTR, PCWSTR, PSID_AND_ATTRIBUTES, DWORD, PSID*);
using DeriveSidFn = HRESULT(WINAPI*)(PCWSTR, PSID*);
using DeleteProfileFn = HRESULT(WINAPI*)(PCWSTR);
using FolderPathFn = HRESULT(WINAPI*)(PCWSTR, PWSTR*);
using CapabilitySidsFn = BOOL(WINAPI*)(LPCWSTR, PSID**, DWORD*, PSID**, DWORD*);

const HRESULT kMissing = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

HRESULT CreateProfile(PCWSTR name, PCWSTR display, PCWSTR description, PSID* sid) {
  static const auto fn = Resolve<CreateProfileFn>(L"userenv.dll", "CreateAppContainerProfile");
  return fn ? fn(name, display, description, nullptr, 0, sid) : kMissing;
}

HRESULT DeriveSid(PCWSTR name, PSID* sid) {
  static const auto fn =
      Resolve<DeriveSidFn>(L"userenv.dll", "DeriveAppContainerSidFromAppContainerName");
  return fn ? fn(name, sid) : kMissing;
}

HRESULT DeleteProfile(PCWSTR name) {
  static const auto fn = Resolve<DeleteProfileFn>(L"userenv.dll", "DeleteAppContainerProfile");
  return fn ? fn(name) : kMissing;
}

HRESULT ContainerFolder(PCWSTR sid, PWSTR* path) {
  static const auto fn = Resolve<FolderPathFn>(L"userenv.dll", "GetAppContainerFolderPath");
  return fn ? fn(sid, path) : kMissing;
}

BOOL CapabilitySids(LPCWSTR name, PSID** groups, DWORD* group_count, PSID** sids,
                    DWORD* sid_count) {
  static const auto fn =
      Resolve<CapabilitySidsFn>(L"kernelbase.dll", "DeriveCapabilitySidsFromName");
  if (fn == nullptr) {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return FALSE;
  }
  return fn(name, groups, group_count, sids, sid_count);
}

std::wstring StringSid(PSID sid) {
  LPWSTR text = nullptr;
  if (!ConvertSidToStringSidW(sid, &text)) return std::wstring();
  std::wstring result(text);
  LocalFree(text);
  return result;
}

// Whether `dacl` already has this explicit allow ACE (no propagation needed again).
bool HasExplicitAce(PACL dacl, PSID trustee, DWORD access, BYTE inherit_flags) {
  if (dacl == nullptr) return false;
  for (DWORD i = 0; i < dacl->AceCount; ++i) {
    void* raw = nullptr;
    if (!GetAce(dacl, i, &raw)) return false;
    auto* header = static_cast<ACE_HEADER*>(raw);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERITED_ACE)) continue;
    auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
    if ((header->AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) != inherit_flags) continue;
    if ((ace->Mask & access) != access) continue;
    if (EqualSid(reinterpret_cast<PSID>(&ace->SidStart), trustee)) return true;
  }
  return false;
}

struct StorageObject {
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~StorageObject() {
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
};

std::wstring WithoutExtendedPrefix(std::wstring path) {
  if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0) path = L"\\\\" + path.substr(8);
  else if (path.compare(0, 4, L"\\\\?\\") == 0) path.erase(0, 4);
  while (path.size() > 3 && path.back() == L'\\') path.pop_back();
  return path;
}

// Storage paths come from the broker's canonical data tree. Pin the object
// before inspecting/changing its DACL, and reject a final or ancestor reparse
// point instead of granting an unrelated target through a mutable pathname.
DWORD OpenStorageObject(const std::wstring& path, StorageObject* object) {
  DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (size == 0) return GetLastError();
  std::vector<wchar_t> full(size);
  DWORD length = GetFullPathNameW(path.c_str(), size, full.data(), nullptr);
  if (length == 0) return GetLastError();
  if (length >= size) return ERROR_INSUFFICIENT_BUFFER;
  size = GetLongPathNameW(full.data(), nullptr, 0);
  if (size == 0) return GetLastError();
  std::vector<wchar_t> expanded(size);
  length = GetLongPathNameW(full.data(), expanded.data(), size);
  if (length == 0) return GetLastError();
  if (length >= size) return ERROR_INSUFFICIENT_BUFFER;
  object->handle = CreateFileW(expanded.data(), READ_CONTROL | WRITE_DAC,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (object->handle == INVALID_HANDLE_VALUE) return GetLastError();
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(object->handle, &info)) return GetLastError();
  if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_CANT_ACCESS_FILE;
  size = GetFinalPathNameByHandleW(object->handle, nullptr, 0, FILE_NAME_NORMALIZED);
  if (size == 0) return GetLastError();
  std::vector<wchar_t> resolved(size + 1);
  length = GetFinalPathNameByHandleW(object->handle, resolved.data(),
                                    static_cast<DWORD>(resolved.size()), FILE_NAME_NORMALIZED);
  if (length == 0) return GetLastError();
  if (length >= resolved.size()) return ERROR_INSUFFICIENT_BUFFER;
  std::wstring expected = WithoutExtendedPrefix(expanded.data());
  std::wstring actual = WithoutExtendedPrefix(std::wstring(resolved.data(), length));
  return CompareStringOrdinal(expected.c_str(), -1, actual.c_str(), -1, TRUE) == CSTR_EQUAL
             ? ERROR_SUCCESS : ERROR_CANT_ACCESS_FILE;
}

// DACL allow/deny ACEs (including object and conditional forms) carry a SID.
// Preserve their bytes instead of flattening ACLs and losing conditional data.
PSID AceTrustee(ACE_HEADER* header) {
  auto* bytes = reinterpret_cast<BYTE*>(header);
  DWORD offset = sizeof(ACE_HEADER) + sizeof(ACCESS_MASK);
  switch (header->AceType) {
    case ACCESS_ALLOWED_ACE_TYPE:
    case ACCESS_DENIED_ACE_TYPE:
    case ACCESS_ALLOWED_CALLBACK_ACE_TYPE:
    case ACCESS_DENIED_CALLBACK_ACE_TYPE:
      break;
    case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
    case ACCESS_DENIED_OBJECT_ACE_TYPE:
    case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE:
    case ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE: {
      if (header->AceSize < offset + sizeof(DWORD)) return nullptr;
      DWORD flags = *reinterpret_cast<DWORD*>(bytes + offset);
      offset += sizeof(DWORD);
      if (flags & ACE_OBJECT_TYPE_PRESENT) offset += sizeof(GUID);
      if (flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) offset += sizeof(GUID);
      break;
    }
    default:
      return nullptr;
  }
  if (header->AceSize < offset + 8) return nullptr;
  auto* sid = reinterpret_cast<PSID>(bytes + offset);
  if (!IsValidSid(sid) || GetLengthSid(sid) > header->AceSize - offset) return nullptr;
  return sid;
}

}  // namespace

Sid::~Sid() {
  if (value == nullptr) return;
  if (local) LocalFree(value);
  else FreeSid(value);
}

bool Enabled() {
  wchar_t value[32];
  DWORD n = GetEnvironmentVariableW(L"GESHER_SANDBOX", value, 32);
  return n > 0 && n < 32 && CompareStringOrdinal(value, static_cast<int>(n), L"appcontainer", -1,
                                                 TRUE) == CSTR_EQUAL;
}

bool ValidName(const std::wstring& name) {
  if (name.empty() || name.size() > 64) return false;
  for (wchar_t c : name) {
    bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
              c == L'.' || c == L'-';
    if (!ok) return false;
  }
  return true;
}

DWORD EnsureProfile(const std::wstring& name, const std::wstring& display_name, Sid* sid) {
  if (!ValidName(name) || sid == nullptr) return ERROR_INVALID_PARAMETER;
  PSID created = nullptr;
  HRESULT hr = CreateProfile(name.c_str(), display_name.c_str(), display_name.c_str(), &created);
  if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) hr = DeriveSid(name.c_str(), &created);
  if (FAILED(hr)) return HRESULT_CODE(hr) != 0 ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE;
  sid->value = created;
  sid->local = false;
  return ERROR_SUCCESS;
}

DWORD ContainerSid(const std::wstring& name, Sid* sid) {
  if (!ValidName(name) || sid == nullptr) return ERROR_INVALID_PARAMETER;
  PSID derived = nullptr;
  HRESULT hr = DeriveSid(name.c_str(), &derived);
  if (FAILED(hr)) return HRESULT_CODE(hr) != 0 ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE;
  sid->value = derived;
  sid->local = false;
  return ERROR_SUCCESS;
}

DWORD DeleteContainerProfile(const std::wstring& name) {
  if (!ValidName(name)) return ERROR_INVALID_PARAMETER;
  HRESULT hr = DeleteProfile(name.c_str());
  // S_OK also when it did not exist (documented); a not-found HRESULT is the same.
  if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
      hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
    return ERROR_SUCCESS;
  }
  return HRESULT_CODE(hr) != 0 ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE;
}

DWORD CapabilitySid(const std::wstring& capability, Sid* sid) {
  if (capability.empty() || sid == nullptr) return ERROR_INVALID_PARAMETER;
  PSID* groups = nullptr;
  PSID* sids = nullptr;
  DWORD group_count = 0, sid_count = 0;
  if (!CapabilitySids(capability.c_str(), &groups, &group_count, &sids, &sid_count)) {
    return GetLastError();
  }
  for (DWORD i = 0; i < group_count; ++i) LocalFree(groups[i]);
  LocalFree(groups);
  if (sid_count == 0) {
    LocalFree(sids);
    return ERROR_INVALID_SID;
  }
  sid->value = sids[0];
  sid->local = true;
  for (DWORD i = 1; i < sid_count; ++i) LocalFree(sids[i]);
  LocalFree(sids);
  return ERROR_SUCCESS;
}

DWORD FolderPath(PSID container, std::wstring* path) {
  if (container == nullptr || path == nullptr) return ERROR_INVALID_PARAMETER;
  std::wstring text = StringSid(container);
  if (text.empty()) return GetLastError();
  PWSTR folder = nullptr;
  HRESULT hr = ContainerFolder(text.c_str(), &folder);
  if (FAILED(hr)) return HRESULT_CODE(hr) != 0 ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE;
  *path = folder;
  CoTaskMemFree(folder);
  return ERROR_SUCCESS;
}

DWORD LocalAppDataPath(std::wstring* path) {
  using KnownFolderFn = HRESULT(WINAPI*)(const GUID*, DWORD, HANDLE, PWSTR*);
  static const auto get = Resolve<KnownFolderFn>(L"shell32.dll", "SHGetKnownFolderPath");
  static constexpr GUID kLocalAppData{0xf1b32785, 0x6fba, 0x4fcf,
                                      {0x9d, 0x55, 0x7b, 0x8e, 0x7f, 0x15, 0x70, 0x91}};
  PWSTR local = nullptr;
  const HRESULT status = get ? get(&kLocalAppData, 0, nullptr, &local) : kMissing;
  if (FAILED(status)) return HRESULT_CODE(status);
  if (!local) return ERROR_PATH_NOT_FOUND;
  *path = local;
  CoTaskMemFree(local);
  return ERROR_SUCCESS;
}

namespace {

struct HeldFile {
  HANDLE handle = nullptr;
  ~HeldFile() { if (handle != nullptr) CloseHandle(handle); }
};

DWORD OpenGrantTarget(const std::wstring& path, DWORD share, bool descendant,
                      HeldFile* open, bool* directory) {
  if (path.empty()) return ERROR_INVALID_PARAMETER;
  const HANDLE file = CreateFileW(path.c_str(), READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
      share, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) return GetLastError();
  HeldFile held{file};
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file, &info)) return GetLastError();
  if (GetFileType(file) != FILE_TYPE_DISK) return ERROR_ACCESS_DENIED;
  *directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      (!*directory && info.nNumberOfLinks != 1)) {
    if (descendant) return ERROR_SUCCESS;
    return ERROR_ACCESS_DENIED;
  }

  // The final component was opened without following a reparse point.
  // Compare the held object's normalized name with the lexical request to
  // detect redirected parent directories, then change only the held object.
  std::wstring opened(32768, L'\0'), expected(32768, L'\0'), long_path(32768, L'\0');
  DWORD count = GetFinalPathNameByHandleW(file, opened.data(), static_cast<DWORD>(opened.size()),
                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (count == 0 || count >= opened.size()) return ERROR_ACCESS_DENIED;
  opened.resize(count);
  count = GetFullPathNameW(path.c_str(), static_cast<DWORD>(expected.size()), expected.data(), nullptr);
  if (count == 0 || count >= expected.size()) return ERROR_ACCESS_DENIED;
  expected.resize(count);
  count = GetLongPathNameW(expected.c_str(), long_path.data(), static_cast<DWORD>(long_path.size()));
  if (count != 0 && count < long_path.size()) expected.assign(long_path.data(), count);
  const auto normalize = [](std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.compare(0, 8, L"\\\\?\\UNC\\") == 0) value = L"\\\\" + value.substr(8);
    else if (value.compare(0, 4, L"\\\\?\\") == 0) value.erase(0, 4);
    while (value.size() > 3 && value.back() == L'\\') value.pop_back();
    return value;
  };
  opened = normalize(std::move(opened));
  expected = normalize(std::move(expected));
  if (expected.size() <= 3 ||
      (expected.compare(0, 2, L"\\\\") == 0 &&
       std::count(expected.begin(), expected.end(), L'\\') < 4) ||
      CompareStringOrdinal(opened.c_str(), -1, expected.c_str(), -1, TRUE) != CSTR_EQUAL) {
    return ERROR_ACCESS_DENIED;
  }
  open->handle = held.handle;
  held.handle = nullptr;
  return ERROR_SUCCESS;
}

DWORD ApplyDacl(HANDLE file, PSECURITY_DESCRIPTOR current, PACL dacl, PSID trustee, DWORD access,
                ACCESS_MODE mode, bool inheritable) {
  EXPLICIT_ACCESS_W entry{};
  entry.grfAccessPermissions = access;
  entry.grfAccessMode = mode;
  entry.grfInheritance = inheritable ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
  entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  entry.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
  entry.Trustee.ptstrName = static_cast<LPWSTR>(trustee);
  PACL updated = nullptr;
  DWORD result = SetEntriesInAclW(1, &entry, dacl, &updated);
  if (result == ERROR_SUCCESS) {
    // Touch only the pinned object. New children inherit the ACE and existing
    // children are visited explicitly so reparse targets aren't traversed.
    SECURITY_DESCRIPTOR replacement{};
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    constexpr SECURITY_DESCRIPTOR_CONTROL preserved = SE_DACL_AUTO_INHERITED | SE_DACL_PROTECTED;
    if (!GetSecurityDescriptorControl(current, &control, &revision) ||
        !InitializeSecurityDescriptor(&replacement, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&replacement, TRUE, updated, (control & SE_DACL_DEFAULTED) != 0) ||
        !SetSecurityDescriptorControl(&replacement, preserved, control & preserved) ||
        !SetKernelObjectSecurity(file, DACL_SECURITY_INFORMATION, &replacement)) {
      result = GetLastError();
    }
    LocalFree(updated);
  }
  return result;
}

DWORD GrantExact(const std::wstring& path, PSID trustee, DWORD access, bool inherit,
                 bool descendant) {
  if (trustee == nullptr || access == 0) return ERROR_INVALID_PARAMETER;
  HeldFile held;
  bool directory = false;
  DWORD error = OpenGrantTarget(path, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 descendant, &held, &directory);
  if (error != ERROR_SUCCESS || held.handle == nullptr) return error;
  HANDLE file = held.handle;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  error = GetSecurityInfo(file, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                          &dacl, nullptr, &descriptor);
  if (error != ERROR_SUCCESS) return error;
  if (dacl == nullptr) {
    LocalFree(descriptor);
    return ERROR_ACCESS_DENIED;
  }
  const bool inheritable = inherit && directory;
  const BYTE flags = inheritable ? (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) : 0;
  if (HasExplicitAce(dacl, trustee, access, flags)) {
    LocalFree(descriptor);
    return ERROR_SUCCESS;
  }
  if (inheritable) {
    // Existing files may contain hard links or directory reparse points. The
    // broker must evaluate exact objects itself before an access grant.
    WIN32_FIND_DATAW entry{};
    const std::wstring prefix = path.back() == L'\\' ? path : path + L'\\';
    const HANDLE search = FindFirstFileW((prefix + L"*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
      error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND) error = ERROR_SUCCESS;
    } else {
      do {
        const std::wstring name(entry.cFileName);
        if (name != L"." && name != L".." &&
            (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
          error = GrantExact(prefix + name, trustee, access, true, true);
        }
        if (error != ERROR_SUCCESS) break;
      } while (FindNextFileW(search, &entry));
      if (error == ERROR_SUCCESS && GetLastError() != ERROR_NO_MORE_FILES) error = GetLastError();
      FindClose(search);
    }
    if (error != ERROR_SUCCESS) {
      LocalFree(descriptor);
      return error;
    }
  }
  error = ApplyDacl(file, descriptor, dacl, trustee, access, GRANT_ACCESS, inheritable);
  LocalFree(descriptor);
  return error;
}

}  // namespace

DWORD Grant(const std::wstring& path, PSID trustee, DWORD access, bool inherit) {
  std::lock_guard<std::mutex> lock(g_acl_lock);
  return GrantExact(path, trustee, access, inherit, false);
}

namespace {
DWORD CheckTreeTrustees(PACL dacl, PSID trustee, const std::vector<PSID>* allowed) {
  if (!dacl) return ERROR_ACCESS_DENIED;
  for (DWORD i = 0; i < dacl->AceCount; ++i) {
    void* raw = nullptr;
    if (!GetAce(dacl, i, &raw)) return GetLastError();
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
    if (ace->Header.AceType == ACCESS_DENIED_ACE_TYPE) continue;
    if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) return ERROR_NOT_SUPPORTED;
    PSID principal = const_cast<DWORD*>(&ace->SidStart);
    if (!IsValidSid(principal)) return ERROR_INVALID_ACL;
    if (allowed) {
      constexpr SID_IDENTIFIER_AUTHORITY app = SECURITY_APP_PACKAGE_AUTHORITY;
      if (std::memcmp(GetSidIdentifierAuthority(principal), &app, sizeof(app)) == 0 &&
          std::none_of(allowed->begin(), allowed->end(),
              [principal](PSID expected) { return EqualSid(principal, expected) != FALSE; }))
        return ERROR_ACCESS_DENIED;
    } else if (ace->Mask && EqualSid(principal, trustee)) return ERROR_ACCESS_DENIED;
  }
  return ERROR_SUCCESS;
}

DWORD RemoveHeldTreeGrants(HANDLE file, PSECURITY_DESCRIPTOR current, PACL dacl, PSID trustee) {
  if (!dacl) return ERROR_ACCESS_DENIED;
  std::vector<BYTE> bytes(dacl->AclSize);
  std::memcpy(bytes.data(), dacl, bytes.size());
  auto* updated = reinterpret_cast<PACL>(bytes.data());
  for (DWORD i = updated->AceCount; i != 0; --i) {
    void* raw = nullptr;
    if (!GetAce(updated, i - 1, &raw)) return GetLastError();
    auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
    if (ace->Header.AceType == ACCESS_DENIED_ACE_TYPE) continue;
    if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) return ERROR_NOT_SUPPORTED;
    if (!IsValidSid(&ace->SidStart)) return ERROR_INVALID_ACL;
    // REVOKE_ACCESS ignores inherited ACEs. Remove the exact allowed trustee
    // ourselves, retaining every other ACE's bytes and relative order.
    if (EqualSid(&ace->SidStart, trustee) && !DeleteAce(updated, i - 1)) return GetLastError();
  }
  SECURITY_DESCRIPTOR replacement{};
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  constexpr SECURITY_DESCRIPTOR_CONTROL preserved = SE_DACL_AUTO_INHERITED | SE_DACL_PROTECTED;
  if (!GetSecurityDescriptorControl(current, &control, &revision) ||
      !InitializeSecurityDescriptor(&replacement, SECURITY_DESCRIPTOR_REVISION) ||
      !SetSecurityDescriptorDacl(&replacement, TRUE, updated, (control & SE_DACL_DEFAULTED) != 0) ||
      !SetSecurityDescriptorControl(&replacement, preserved, control & preserved) ||
      !SetKernelObjectSecurity(file, DACL_SECURITY_INFORMATION, &replacement)) return GetLastError();
  return ERROR_SUCCESS;
}

DWORD RevokeTreeExact(const std::wstring& path, PSID trustee, unsigned depth, unsigned* remaining,
                     bool remove, const std::vector<PSID>* allowed = nullptr) {
  if (!depth || !*remaining) return ERROR_NOT_ENOUGH_QUOTA;
  --*remaining;
  HeldFile held;
  bool directory = false;
  DWORD error = OpenGrantTarget(path, FILE_SHARE_READ | FILE_SHARE_WRITE, false, &held, &directory);
  if (error != ERROR_SUCCESS || !held.handle) return error != ERROR_SUCCESS ? error : ERROR_ACCESS_DENIED;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  error = GetSecurityInfo(held.handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
      nullptr, nullptr, &dacl, nullptr, &descriptor);
  if (error != ERROR_SUCCESS) return error;
  // Remove the parent first, so the kernel cannot regenerate the old inherited
  // grant when each child's exact DACL is replaced. Never propagate implicitly.
  error = remove ? RemoveHeldTreeGrants(held.handle, descriptor, dacl, trustee) :
      CheckTreeTrustees(dacl, trustee, allowed);
  LocalFree(descriptor);
  if (error != ERROR_SUCCESS) return error;
  if (directory) {
    WIN32_FIND_DATAW entry{};
    const std::wstring prefix = path.back() == L'\\' ? path : path + L'\\';
    HANDLE search = FindFirstFileW((prefix + L"*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
      if (GetLastError() != ERROR_FILE_NOT_FOUND) return GetLastError();
    } else {
      do {
        const std::wstring name(entry.cFileName);
        if (name != L"." && name != L"..") error = RevokeTreeExact(prefix + name, trustee, depth - 1, remaining, remove, allowed);
        if (error != ERROR_SUCCESS) break;
      } while (FindNextFileW(search, &entry));
      const DWORD last = GetLastError();
      FindClose(search);
      if (error != ERROR_SUCCESS) return error;
      if (last != ERROR_NO_MORE_FILES) return last;
    }
  }
  if (remove) {
    descriptor = nullptr; dacl = nullptr;
    error = GetSecurityInfo(held.handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, &descriptor);
    if (error == ERROR_SUCCESS) error = CheckTreeTrustees(dacl, trustee, nullptr);
    LocalFree(descriptor);
  }
  return error;
}

}  // namespace

DWORD RevokeTree(const std::wstring& path, PSID trustee) {
  if (!trustee || !IsValidSid(trustee)) return ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(g_acl_lock);
  unsigned remaining = 100000;
  return RevokeTreeExact(path, trustee, 64, &remaining, true);
}

DWORD RequireNoGrantTree(const std::wstring& path, PSID trustee) {
  if (!trustee || !IsValidSid(trustee)) return ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(g_acl_lock);
  unsigned remaining = 100000;
  return RevokeTreeExact(path, trustee, 64, &remaining, false);
}

DWORD RequireAppTrusteesTree(const std::wstring& path, const std::vector<PSID>& allowed) {
  if (allowed.empty() || std::any_of(allowed.begin(), allowed.end(), [](PSID sid) { return !sid || !IsValidSid(sid); }))
    return ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(g_acl_lock);
  unsigned remaining = 100000;
  return RevokeTreeExact(path, nullptr, 64, &remaining, false, &allowed);
}

DWORD Revoke(const std::wstring& path, PSID trustee) {
  return SetStorageAccess(path, trustee, 0, false);
}

DWORD SetStorageAccess(const std::wstring& path, PSID trustee, DWORD access, bool inherit) {
  if (path.empty() || trustee == nullptr) return ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(g_acl_lock);
  StorageObject object;
  DWORD error = OpenStorageObject(path, &object);
  if (error != ERROR_SUCCESS) return error;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  error = GetSecurityInfo(object.handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                          nullptr, nullptr, &dacl, nullptr, &descriptor);
  if (error != ERROR_SUCCESS) return error;
  // A null DACL grants everyone access. Do not pretend a per-package entry can
  // enforce the policy in that case, or discard the owner's existing access.
  if (dacl == nullptr) {
    LocalFree(descriptor);
    return ERROR_INVALID_ACL;
  }
  EXPLICIT_ACCESS_W entry{};
  entry.grfAccessPermissions = access;
  entry.grfAccessMode = access == 0 ? REVOKE_ACCESS : SET_ACCESS;
  entry.grfInheritance = access != 0 && inherit
                            ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
  entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  entry.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
  entry.Trustee.ptstrName = static_cast<LPWSTR>(trustee);
  PACL updated = nullptr;
  error = SetEntriesInAclW(1, &entry, dacl, &updated);
  if (error == ERROR_SUCCESS) {
    // Always propagate storage changes. In particular a prior Modify ACE must
    // not satisfy a new Read request just because it contains all Read bits.
    error = SetSecurityInfo(object.handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                             nullptr, nullptr, updated, nullptr);
    LocalFree(updated);
  }
  LocalFree(descriptor);
  return error;
}

DWORD ProtectPrivateStorage(const std::wstring& path) {
  if (path.empty()) return ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(g_acl_lock);
  StorageObject object;
  DWORD error = OpenStorageObject(path, &object);
  if (error != ERROR_SUCCESS) return error;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  error = GetSecurityInfo(object.handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                          nullptr, nullptr, &dacl, nullptr, &descriptor);
  if (error != ERROR_SUCCESS) return error;
  if (dacl == nullptr) {
    LocalFree(descriptor);
    return ERROR_INVALID_ACL;
  }
  std::vector<BYTE> bytes(dacl->AclSize);
  auto* protected_dacl = reinterpret_cast<PACL>(bytes.data());
  if (!InitializeAcl(protected_dacl, dacl->AclSize, dacl->AclRevision)) {
    error = GetLastError();
  } else {
    SID_IDENTIFIER_AUTHORITY packages = SECURITY_APP_PACKAGE_AUTHORITY;
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
      void* raw = nullptr;
      if (!GetAce(dacl, i, &raw)) {
        error = GetLastError();
        break;
      }
      auto* ace = static_cast<ACE_HEADER*>(raw);
      PSID sid = AceTrustee(ace);
      if (sid == nullptr) {
        error = ERROR_INVALID_ACL;
        break;
      }
      // No package or capability may inherit the shared-root grant through
      // Android/data or Android/obb. Retain Windows user/SYSTEM access; the
      // own-package grants on child directories remain explicit and untouched.
      if (std::memcmp(GetSidIdentifierAuthority(sid), &packages, sizeof(packages)) == 0) continue;
      std::vector<BYTE> copy(reinterpret_cast<BYTE*>(raw),
                             reinterpret_cast<BYTE*>(raw) + ace->AceSize);
      reinterpret_cast<ACE_HEADER*>(copy.data())->AceFlags &= ~INHERITED_ACE;
      if (!AddAce(protected_dacl, dacl->AclRevision, MAXDWORD, copy.data(), ace->AceSize)) {
        error = GetLastError();
        break;
      }
    }
    if (error == ERROR_SUCCESS) {
      error = SetSecurityInfo(object.handle, SE_FILE_OBJECT,
                               DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                               nullptr, nullptr, protected_dacl, nullptr);
    }
  }
  LocalFree(descriptor);
  return error;
}

DWORD BinderPipeSecurity(PSECURITY_DESCRIPTOR* descriptor) {
  Sid capability;
  const DWORD error = CapabilitySid(kBinderClientCapability, &capability);
  return error != ERROR_SUCCESS ? error : AppContainerPipeSecurity(descriptor, capability.value);
}

DWORD AppContainerPipeSecurity(PSECURITY_DESCRIPTOR* descriptor, PSID trustee) {
  if (descriptor == nullptr) return ERROR_INVALID_PARAMETER;
  if (!trustee || !IsValidSid(trustee)) return ERROR_INVALID_SID;
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return GetLastError();
  alignas(TOKEN_USER) BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  DWORD size = 0;
  BOOL ok = GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size);
  DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(token);
  if (!ok) return error;
  std::wstring user = StringSid(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid);
  std::wstring client = StringSid(trustee);
  if (user.empty() || client.empty()) return ERROR_INVALID_SID;
  // Protected DACL: nothing inherited, no Everyone/anonymous read of the default one.
  // The client gets FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES
  // (0x12018b), what binder_wire.h's kPipeClientAccess asks: no FILE_APPEND_DATA,
  // which for a pipe is FILE_CREATE_PIPE_INSTANCE (serving an instance of it).
  std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + user + L")(A;;0x12018b;;;" + client +
                      L")S:(ML;;NW;;;LW)";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                            descriptor, nullptr)) {
    return GetLastError();
  }
  return ERROR_SUCCESS;
}

}  // namespace appcontainer
}  // namespace gesher
