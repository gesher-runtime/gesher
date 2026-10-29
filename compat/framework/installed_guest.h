// Host-owned install generation policy for the first migrated package route.
#pragma once
#include <windows.h>
#include <winternl.h>
#include <aclapi.h>
#include <userenv.h>
#include <memory>
#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <cstddef>
#include <cstdio>
#include <array>
#include <atomic>
#include <string>
#include <vector>
#include "../binder/launch_control.h"

namespace gesher::guest {
inline constexpr char16_t kSelectedPackage[] = u"com.gesher.lifecycle";

// Only the dedicated system server constructs these, from PackageManager's
// installed entry. No environment variable or Binder hello provisions a policy.
struct InstalledPolicy {
  PSID sid = nullptr;
  std::wstring profile;
  std::u16string package, install, executable;
  int uid = -1;
  bool retiring = false;
  std::vector<std::wstring> grants;
  struct Pin { std::wstring path; HANDLE handle; bool directory = false; bool writable = false; };
  std::vector<Pin> pins;
  std::vector<HANDLE> ancestors;
  HMODULE userenv = nullptr;
  struct Label { HANDLE handle; PSECURITY_DESCRIPTOR original; };
  std::vector<Label> labels;
  using DeleteProfile = HRESULT (WINAPI*)(PCWSTR);
  DeleteProfile delete_profile = nullptr;
  decltype(&SetSecurityInfo) set_security_ = SetSecurityInfo; // fixture fault seam, no JNI setter
  std::atomic<InstalledPolicy*>* slot = nullptr;
  static std::array<std::atomic<InstalledPolicy*>, 64>& Slots() {
    static std::array<std::atomic<InstalledPolicy*>, 64> slots{}; return slots;
  }
  static InstalledPolicy* Busy() { return reinterpret_cast<InstalledPolicy*>(uintptr_t(1)); }
  static void Release(InstalledPolicy* policy) {
    if (policy->Retire()) {
      if (policy->slot) policy->slot->store(nullptr, std::memory_order_release);
      delete policy;
    } else {
      // Retain profile, SID, exact ACL paths and label for bounded supervisor
      // retry. Never classify failed resource retirement as completed uninstall.
      policy->slot->store(policy, std::memory_order_release);
      OutputDebugStringW(L"Gesher installed policy retirement unconfirmed\n");
    }
  }
  static void RetryRetired() {
    for (auto& slot : Slots()) {
      auto* policy = slot.load(std::memory_order_acquire);
      if (!policy || policy == Busy()) continue;
      if (slot.compare_exchange_strong(policy, Busy(), std::memory_order_acquire)) Release(policy);
    }
  }
  bool Retire() {
    retiring = true;
    bool ok = true;
    for (auto it = grants.begin(); it != grants.end();) {
      if (Grant(*it, 0, REVOKE_ACCESS)) it = grants.erase(it);
      else { ok = false; ++it; }
    }
    for (auto it = labels.begin(); it != labels.end();) {
      PACL label = nullptr; BOOL present = FALSE, defaulted = FALSE;
      if (Deleted(it->handle) || (GetSecurityDescriptorSacl(it->original, &present, &label, &defaulted) &&
          set_security_(it->handle, SE_FILE_OBJECT,
            LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, label) == ERROR_SUCCESS)) {
        LocalFree(it->original); it = labels.erase(it);
      } else { ok = false; ++it; }
    }
    if (ok && delete_profile && !profile.empty()) {
      if (SUCCEEDED(delete_profile(profile.c_str()))) profile.clear();
      else ok = false;
    }
    return ok;
  }
  ~InstalledPolicy() {
    for (const auto& pin : pins) CloseHandle(pin.handle);
    for (HANDLE handle : ancestors) CloseHandle(handle);
    if (sid) FreeSid(sid);
    if (userenv) FreeLibrary(userenv);
  }
  HANDLE Pinned(const std::wstring& path) const {
    for (const auto& pin : pins) if (pin.path == path) return pin.handle;
    return INVALID_HANDLE_VALUE;
  }
  bool PinResource(const std::wstring& path, bool writable = false) {
    if (Pinned(path) != INVALID_HANDLE_VALUE) return true;
    for (size_t i = 3; i <= path.size(); ++i) {
      if (i != 3 && i != path.size() && path[i] != L'\\') continue;
      bool final = i == path.size();
      if (!final && Pinned(path.substr(0, i)) != INVALID_HANDLE_VALUE) continue;
      DWORD attributes = final ? GetFileAttributesW(path.c_str()) : FILE_ATTRIBUTE_DIRECTORY;
      if (attributes == INVALID_FILE_ATTRIBUTES) return false;
      bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      HANDLE handle = CreateFileW(path.substr(0, i).c_str(),
          final && directory ? MAXIMUM_ALLOWED : FILE_READ_DATA | FILE_READ_ATTRIBUTES | (final ? READ_CONTROL | WRITE_DAC | WRITE_OWNER : 0),
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
      if (handle == INVALID_HANDLE_VALUE) return false;
      FILE_ATTRIBUTE_TAG_INFO tag{};
      if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
          (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
          directory != bool(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { CloseHandle(handle); return false; }
      // No DELETE sharing: each already opened ancestor remains the same object
      // while resolving its child, and no root can be replaced before cleanup.
      if (final) pins.push_back({path, handle, directory, writable});
      else ancestors.push_back(handle);
    }
    return Pinned(path) != INVALID_HANDLE_VALUE;
  }
  static bool Deleted(HANDLE handle) {
    FILE_STANDARD_INFO info{};
    return GetFileInformationByHandleEx(handle, FileStandardInfo, &info, sizeof(info)) && info.DeletePending;
  }
  // Enumeration is observational, not a promise that no concurrent entry can
  // appear. Each opened child is resolved relative to its retained directory,
  // with a single-component name and OPEN_REPARSE_POINT. Host mutations below
  // always address these exact handles and never recurse through a pathname.
  bool Gather(size_t index, unsigned depth = 0) {
    if (depth > 128 || pins.size() > 65536) return false;
    Pin parent = pins[index];
    if (!parent.directory) {
      FILE_STANDARD_INFO info{};
      return GetFileInformationByHandleEx(parent.handle, FileStandardInfo, &info, sizeof(info)) && info.NumberOfLinks == 1;
    }
    HMODULE library = GetModuleHandleW(L"ntdll.dll");
    if (!library) return false;
    auto open = reinterpret_cast<decltype(&NtCreateFile)>(GetProcAddress(library, "NtCreateFile"));
    if (!open) return false;
    std::vector<BYTE> buffer(65536);
    bool first = true;
    for (;;) {
      BOOL result = GetFileInformationByHandleEx(parent.handle,
          first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
          buffer.data(), static_cast<DWORD>(buffer.size()));
      first = false;
      if (!result) return GetLastError() == ERROR_NO_MORE_FILES;
      size_t offset = 0;
      for (;;) {
        if (offset + offsetof(FILE_ID_BOTH_DIR_INFO, FileName) > buffer.size()) return false;
        auto* item = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
        if (item->FileNameLength % sizeof(wchar_t) ||
            item->FileNameLength > buffer.size() - offset - offsetof(FILE_ID_BOTH_DIR_INFO, FileName)) return false;
        std::wstring name(item->FileName, item->FileNameLength / sizeof(wchar_t));
        if (name != L"." && name != L"..") {
          if (name.empty() || name.find_first_of(L"\\/:") != std::wstring::npos ||
              (item->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || pins.size() >= 65536) return false;
          bool directory = (item->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
          UNICODE_STRING unicode{};
          unicode.Buffer = name.data(); unicode.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
          unicode.MaximumLength = unicode.Length;
          OBJECT_ATTRIBUTES attributes{};
          attributes.Length = sizeof(attributes); attributes.RootDirectory = parent.handle;
          attributes.ObjectName = &unicode; attributes.Attributes = 0x40; // OBJ_CASE_INSENSITIVE
          IO_STATUS_BLOCK io{}; HANDLE child = nullptr;
          NTSTATUS status = open(&child, directory ? MAXIMUM_ALLOWED :
              FILE_READ_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC | WRITE_OWNER | SYNCHRONIZE,
              &attributes, &io, nullptr, 0,
              FILE_SHARE_READ | FILE_SHARE_WRITE | (parent.writable ? FILE_SHARE_DELETE : 0),
              1, 0x00200000 | 0x00004000 | 0x00000020, nullptr, 0);
          if (status < 0 || !child) return false;
          FILE_ATTRIBUTE_TAG_INFO tag{};
          if (!GetFileInformationByHandleEx(child, FileAttributeTagInfo, &tag, sizeof(tag)) ||
              (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
              directory != bool(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { CloseHandle(child); return false; }
          size_t child_index = pins.size();
          pins.push_back({parent.path + L"\\" + name, child, directory, parent.writable});
          if (!Gather(child_index, depth + 1)) return false;
        }
        if (!item->NextEntryOffset) break;
        if (item->NextEntryOffset < offsetof(FILE_ID_BOTH_DIR_INFO, FileName) ||
            item->NextEntryOffset > buffer.size() - offset) return false;
        offset += item->NextEntryOffset;
      }
    }
  }
  bool Grant(const std::wstring& path, DWORD rights, ACCESS_MODE mode = GRANT_ACCESS) {
    PACL old_acl = nullptr, acl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    HANDLE handle = Pinned(path);
    if (handle == INVALID_HANDLE_VALUE) return false;
    if (mode == REVOKE_ACCESS && Deleted(handle)) return true;
    DWORD error = GetSecurityInfo(handle, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_acl, nullptr, &sd);
    if (error != ERROR_SUCCESS) return false;
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = rights;
    entry.grfAccessMode = mode;
    // Directory handles were opened MAXIMUM_ALLOWED. SetSecurityInfo therefore
    // does not propagate to existing children (documented API contract); newly
    // created objects receive this install's intended inheritance. Files have
    // no descendants. Never substitute a metadata-only directory handle here.
    entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    entry.Trustee.ptstrName = static_cast<wchar_t*>(sid);
    error = SetEntriesInAclW(1, &entry, old_acl, &acl);
    if (error == ERROR_SUCCESS) error = set_security_(
        handle, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    if (acl) LocalFree(acl);
    if (sd) LocalFree(sd);
    return error == ERROR_SUCCESS;
  }
  static bool ReadOnlyIntegrity(HANDLE handle) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL label = nullptr;
    if (GetSecurityInfo(handle, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, &label, &descriptor) != ERROR_SUCCESS) return false;
    bool ok = true;
    unsigned mandatory = 0;
    if (label) for (DWORD i = 0; i < label->AceCount; ++i) {
      void* bytes = nullptr;
      if (!GetAce(label, i, &bytes)) { ok = false; break; }
      auto* ace = static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(bytes);
      if (ace->Header.AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
      PSID sid = &ace->SidStart;
      if (++mandatory != 1 || !IsValidSid(sid) || !*GetSidSubAuthorityCount(sid) ||
          *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1) < SECURITY_MANDATORY_MEDIUM_RID ||
          !(ace->Mask & SYSTEM_MANDATORY_LABEL_NO_WRITE_UP)) { ok = false; break; }
    }
    // An object without an explicit mandatory label has medium integrity/NW.
    LocalFree(descriptor);
    return ok;
  }
  // Reject relative paths and reparse components, including a junction parent.
  static std::wstring Path(const std::u16string& input) {
    std::wstring path(reinterpret_cast<const wchar_t*>(input.c_str()));
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.size() < 4 || path[1] != L':' || path[2] != L'\\' ||
        path.find(L"..") != std::wstring::npos || path.find(L':', 2) != std::wstring::npos)
      return {};
    for (size_t i = 3; i <= path.size(); ++i) {
      if (i != path.size() && path[i] != L'\\') continue;
      DWORD attrs = GetFileAttributesW(path.substr(0, i).c_str());
      if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    }
    return path;
  }
  static std::shared_ptr<InstalledPolicy> Create(const std::u16string& package,
      const std::u16string& install, int uid, const std::u16string& executable,
      const std::vector<std::u16string>& read, const std::u16string& data) {
    // Report a bounded stage without resource paths or identity data. Some
    // validation failures have no Win32 error; zero means unavailable.
    auto failed = [](const char* stage, unsigned long error = 0) {
      std::fprintf(stderr, "installed guest policy: stage=%s error=%lu\n", stage, error);
      return std::shared_ptr<InstalledPolicy>{};
    };
    launch_control::Request identity;
    if (package != kSelectedPackage || uid < 10000 || uid > 19999 ||
        !launch_control::Generation(install, identity.generation) || read.empty()) return failed("identity");
    RetryRetired();
    auto policy = std::shared_ptr<InstalledPolicy>(new InstalledPolicy(), Release);
    for (auto& slot : Slots()) {
      InstalledPolicy* expected = nullptr;
      if (slot.compare_exchange_strong(expected, Busy(), std::memory_order_acquire)) { policy->slot = &slot; break; }
    }
    if (!policy->slot) return failed("slot");
    policy->package = package; policy->install = install; policy->uid = uid;
    std::wstring exe = Path(executable), writable = Path(data);
    if (exe.empty() || writable.empty()) return failed("executable-or-data-path");
    policy->executable.assign(reinterpret_cast<const char16_t*>(exe.c_str()));
    std::vector<std::wstring> resources;
    // Runtime files beside this exact host executable; never grant its parent
    // directory recursively (installed layouts put private data under it).
    std::wstring runtime = exe.substr(0, exe.find_last_of(L'\\'));
    WIN32_FIND_DATAW file{};
    HANDLE scan = FindFirstFileW((runtime + L"\\*").c_str(), &file);
    if (scan == INVALID_HANDLE_VALUE) return failed("runtime-scan", GetLastError());
    do {
      if (file.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
      std::wstring name = file.cFileName;
      auto dot = name.find_last_of(L'.');
      std::wstring ext = dot == std::wstring::npos ? L"" : name.substr(dot);
      if (ext == L".dll" || ext == L".jar" || ext == L".dat") resources.push_back(runtime + L"\\" + name);
    } while (FindNextFileW(scan, &file));
    DWORD scan_error = GetLastError();
    FindClose(scan);
    if (scan_error != ERROR_NO_MORE_FILES) return failed("runtime-scan-next", scan_error);
    std::wstring framework = runtime + L"\\framework";
    DWORD framework_attributes = GetFileAttributesW(framework.c_str());
    if (framework_attributes != INVALID_FILE_ATTRIBUTES) {
      auto checked = Path(std::u16string(reinterpret_cast<const char16_t*>(framework.c_str())));
      if (checked.empty() || !(framework_attributes & FILE_ATTRIBUTE_DIRECTORY)) return failed("framework-path");
      resources.push_back(std::move(checked));
    }
    for (const auto& item : read) {
      auto path = Path(item);
      if (path.empty()) return failed("read-path");
      // No read tree can encompass app data (or vice versa).
      std::wstring a = path + L'\\', b = writable + L'\\';
      std::transform(a.begin(), a.end(), a.begin(), towlower);
      std::transform(b.begin(), b.end(), b.begin(), towlower);
      if (a.compare(0, b.size(), b) == 0 || b.compare(0, a.size(), a) == 0) return failed("read-data-overlap");
      resources.push_back(std::move(path));
    }
    policy->profile = launch_control::Profile(identity.generation);
    policy->userenv = LoadLibraryW(L"userenv.dll");
    if (!policy->userenv) return failed("userenv-load", GetLastError());
    using CreateProfile = HRESULT (WINAPI*)(PCWSTR, PCWSTR, PCWSTR, PSID_AND_ATTRIBUTES, DWORD, PSID*);
    auto create = reinterpret_cast<CreateProfile>(GetProcAddress(policy->userenv, "CreateAppContainerProfile"));
    policy->delete_profile = reinterpret_cast<DeleteProfile>(GetProcAddress(policy->userenv, "DeleteAppContainerProfile"));
    if (!create || !policy->delete_profile) return failed("userenv-exports", GetLastError());
    HRESULT result = create(policy->profile.c_str(), policy->profile.c_str(), L"Gesher installed guest", nullptr, 0, &policy->sid);
    // Existing profiles are not adopted: their ownership/lifetime is unknown.
    if (FAILED(result)) { policy->profile.clear(); return failed("profile-create-hresult", static_cast<unsigned long>(result)); }
    resources.push_back(exe);
    for (const auto& path : resources) {
      if (policy->Pinned(path) != INVALID_HANDLE_VALUE) continue;
      size_t root = policy->pins.size();
      if (!policy->PinResource(path)) return failed("resource-pin");
      if (!policy->Gather(root)) return failed("resource-gather");
    }
    size_t data_root = policy->pins.size();
    if (!policy->PinResource(writable, true)) return failed("data-pin");
    if (!policy->Gather(data_root)) return failed("data-gather");
    for (const auto& pin : policy->pins)
      if (!pin.writable && !ReadOnlyIntegrity(pin.handle)) return failed("resource-integrity");
    PSECURITY_DESCRIPTOR low = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;OICI;NW;;;LW)",
        SDDL_REVISION_1, &low, nullptr)) return failed("label-descriptor", GetLastError());
    PACL label = nullptr; BOOL present = FALSE, defaulted = FALSE;
    bool labeled = GetSecurityDescriptorSacl(low, &present, &label, &defaulted) != FALSE;
    for (const auto& pin : policy->pins) {
      policy->grants.push_back(pin.path);
      DWORD rights = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE |
          (pin.writable ? FILE_GENERIC_WRITE | DELETE : 0);
      if (!policy->Grant(pin.path, rights)) { labeled = false; break; }
      if (pin.writable) {
        PSECURITY_DESCRIPTOR original = nullptr;
        if (GetSecurityInfo(pin.handle, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
            nullptr, nullptr, nullptr, nullptr, &original) != ERROR_SUCCESS) { labeled = false; break; }
        policy->labels.push_back({pin.handle, original});
        if (policy->set_security_(pin.handle, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
            nullptr, nullptr, nullptr, label) != ERROR_SUCCESS) { labeled = false; break; }
      }
    }
    LocalFree(low);
    if (!labeled) return failed("grant-or-label");
    return policy;
  }
};
}
