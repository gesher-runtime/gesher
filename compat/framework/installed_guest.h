// Host-owned install generation policy for the first migrated package route.
#pragma once
#include <windows.h>
#include <aclapi.h>
#include <userenv.h>
#include <memory>
#include <algorithm>
#include <cwctype>
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
  std::vector<std::wstring> grants;
  HMODULE userenv = nullptr;
  std::wstring writable_path;
  PSECURITY_DESCRIPTOR old_label = nullptr;
  using DeleteProfile = HRESULT (WINAPI*)(PCWSTR);
  DeleteProfile delete_profile = nullptr;
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
    bool ok = true;
    for (auto it = grants.begin(); it != grants.end();) {
      if (Grant(*it, 0, REVOKE_ACCESS)) it = grants.erase(it);
      else { ok = false; ++it; }
    }
    if (old_label) {
      PACL label = nullptr; BOOL present = FALSE, defaulted = FALSE;
      if (GetSecurityDescriptorSacl(old_label, &present, &label, &defaulted) &&
          SetNamedSecurityInfoW(writable_path.data(), SE_FILE_OBJECT,
            LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, label) == ERROR_SUCCESS) {
        LocalFree(old_label); old_label = nullptr;
      } else ok = false;
    }
    if (ok && delete_profile && !profile.empty()) {
      if (SUCCEEDED(delete_profile(profile.c_str()))) profile.clear();
      else ok = false;
    }
    return ok;
  }
  ~InstalledPolicy() {
    if (sid) FreeSid(sid);
    if (userenv) FreeLibrary(userenv);
  }
  bool Grant(const std::wstring& path, DWORD rights, ACCESS_MODE mode = GRANT_ACCESS) {
    PACL old_acl = nullptr, acl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD error = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_acl, nullptr, &sd);
    if (error != ERROR_SUCCESS) return false;
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = rights;
    entry.grfAccessMode = mode;
    entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    entry.Trustee.ptstrName = static_cast<wchar_t*>(sid);
    error = SetEntriesInAclW(1, &entry, old_acl, &acl);
    if (error == ERROR_SUCCESS) error = SetNamedSecurityInfoW(
        const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    if (acl) LocalFree(acl);
    if (sd) LocalFree(sd);
    return error == ERROR_SUCCESS;
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
    launch_control::Request identity;
    if (package != kSelectedPackage || uid < 10000 || uid > 19999 ||
        !launch_control::Generation(install, identity.generation) || read.empty()) return {};
    RetryRetired();
    auto policy = std::shared_ptr<InstalledPolicy>(new InstalledPolicy(), Release);
    for (auto& slot : Slots()) {
      InstalledPolicy* expected = nullptr;
      if (slot.compare_exchange_strong(expected, Busy(), std::memory_order_acquire)) { policy->slot = &slot; break; }
    }
    if (!policy->slot) return {};
    policy->package = package; policy->install = install; policy->uid = uid;
    std::wstring exe = Path(executable), writable = Path(data);
    if (exe.empty() || writable.empty()) return {};
    policy->executable.assign(reinterpret_cast<const char16_t*>(exe.c_str()));
    std::vector<std::wstring> resources;
    // Runtime files beside this exact host executable; never grant its parent
    // directory recursively (installed layouts put private data under it).
    std::wstring runtime = exe.substr(0, exe.find_last_of(L'\\'));
    WIN32_FIND_DATAW file{};
    HANDLE scan = FindFirstFileW((runtime + L"\\*").c_str(), &file);
    if (scan == INVALID_HANDLE_VALUE) return {};
    do {
      if (file.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
      std::wstring name = file.cFileName;
      auto dot = name.find_last_of(L'.');
      std::wstring ext = dot == std::wstring::npos ? L"" : name.substr(dot);
      if (ext == L".dll" || ext == L".jar" || ext == L".dat") resources.push_back(runtime + L"\\" + name);
    } while (FindNextFileW(scan, &file));
    DWORD scan_error = GetLastError();
    FindClose(scan);
    if (scan_error != ERROR_NO_MORE_FILES) return {};
    for (const auto& item : read) {
      auto path = Path(item);
      if (path.empty()) return {};
      // No read tree can encompass app data (or vice versa).
      std::wstring a = path + L'\\', b = writable + L'\\';
      std::transform(a.begin(), a.end(), a.begin(), towlower);
      std::transform(b.begin(), b.end(), b.begin(), towlower);
      if (a.compare(0, b.size(), b) == 0 || b.compare(0, a.size(), a) == 0) return {};
      resources.push_back(std::move(path));
    }
    policy->profile = launch_control::Profile(identity.generation);
    policy->userenv = LoadLibraryW(L"userenv.dll");
    if (!policy->userenv) return {};
    using CreateProfile = HRESULT (WINAPI*)(PCWSTR, PCWSTR, PCWSTR, PSID_AND_ATTRIBUTES, DWORD, PSID*);
    auto create = reinterpret_cast<CreateProfile>(GetProcAddress(policy->userenv, "CreateAppContainerProfile"));
    policy->delete_profile = reinterpret_cast<DeleteProfile>(GetProcAddress(policy->userenv, "DeleteAppContainerProfile"));
    if (!create || !policy->delete_profile) return {};
    HRESULT result = create(policy->profile.c_str(), policy->profile.c_str(), L"Gesher installed guest", nullptr, 0, &policy->sid);
    // Existing profiles are not adopted: their ownership/lifetime is unknown.
    if (FAILED(result)) { policy->profile.clear(); return {}; }
    resources.push_back(exe);
    for (const auto& path : resources) {
      policy->grants.push_back(path);
      if (!policy->Grant(path, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE)) return {};
    }
    policy->grants.push_back(writable);
    if (!policy->Grant(writable, FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE)) return {};
    policy->writable_path = writable;
    if (GetNamedSecurityInfoW(writable.c_str(), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &policy->old_label) != ERROR_SUCCESS) return {};
    PSECURITY_DESCRIPTOR low = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;OICI;NW;;;LW)",
        SDDL_REVISION_1, &low, nullptr)) return {};
    PACL label = nullptr; BOOL present = FALSE, defaulted = FALSE;
    bool labeled = GetSecurityDescriptorSacl(low, &present, &label, &defaulted) &&
        SetNamedSecurityInfoW(writable.data(), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
            nullptr, nullptr, nullptr, label) == ERROR_SUCCESS;
    LocalFree(low);
    if (!labeled) return {};
    return policy;
  }
};
}
