// Gesher: Windows shell integration of installed Android apps, the Win32 side
// (see shell_win32.h).
//
// The COM and shell headers need what WIN32_LEAN_AND_MEAN and NOGDI (the
// build's defaults) leave out.
#undef WIN32_LEAN_AND_MEAN
#undef NOGDI
#include <windows.h>
#include <objbase.h>
#include <objidl.h>
#include <propidl.h>
#include <propsys.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstring>
#include <string>

#include "shell_win32.h"

namespace gesher {
namespace shell {
namespace {

const wchar_t* W(const std::u16string& s) { return reinterpret_cast<const wchar_t*>(s.c_str()); }

std::u16string U(const wchar_t* s) {
  return s == nullptr ? std::u16string() : std::u16string(reinterpret_cast<const char16_t*>(s));
}

// The GUIDs, spelled out: not every one is in llvm-mingw's libuuid.
const CLSID kClsidShellLink = {0x00021401, 0x0000, 0x0000,
                               {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIidShellLinkW = {0x000214F9, 0x0000, 0x0000,
                            {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIidPersistFile = {0x0000010B, 0x0000, 0x0000,
                             {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIidPropertyStore = {0x886D8EEB, 0x8CF2, 0x4446,
                               {0x8D, 0x02, 0xCD, 0xBA, 0x1D, 0xBD, 0xCF, 0x99}};
// PKEY_AppUserModel_ID (propkey.h).
const PROPERTYKEY kAppUserModelId = {
    {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};
// FOLDERID_Programs (knownfolders.h).
const GUID kFolderIdPrograms = {0xA77F5D77, 0x2E2B, 0x44C3,
                                {0xA6, 0xA2, 0xAB, 0xA6, 0x01, 0x05, 0x4A, 0x51}};

// shell32's functions are looked up when first used: dalvikvm does not link
// shell32, and most of its processes never need it.
void* Shell32(const char* name) {
  static HMODULE module = LoadLibraryExW(L"shell32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  return module == nullptr ? nullptr : reinterpret_cast<void*>(GetProcAddress(module, name));
}

using SHGetKnownFolderPathFn = HRESULT(WINAPI*)(const GUID*, DWORD, HANDLE, PWSTR*);
using SHChangeNotifyFn = void(WINAPI*)(LONG, UINT, LPCVOID, LPCVOID);
using SetAppIdFn = HRESULT(WINAPI*)(PCWSTR);
using GetAppIdFn = HRESULT(WINAPI*)(PWSTR*);

// COM on this thread for the scope: a single-threaded apartment, or the
// multithreaded one the thread already joined (the shell link object
// lives in both).
class ComScope {
 public:
  ComScope() : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
  ~ComScope() {
    if (SUCCEEDED(hr_)) CoUninitialize();
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
  bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }
  HRESULT hr() const { return hr_; }

 private:
  HRESULT hr_;
};

template <typename T>
class Ref {
 public:
  Ref() = default;
  ~Ref() {
    if (p_ != nullptr) p_->Release();
  }
  Ref(const Ref&) = delete;
  Ref& operator=(const Ref&) = delete;
  T* operator->() const { return p_; }
  T* get() const { return p_; }
  void** out() { return reinterpret_cast<void**>(&p_); }

 private:
  T* p_ = nullptr;
};

HRESULT SetAppId(IShellLinkW* link, const std::u16string& app_id) {
  Ref<IPropertyStore> store;
  HRESULT hr = link->QueryInterface(kIidPropertyStore, store.out());
  if (FAILED(hr)) return hr;
  PROPVARIANT value;
  std::memset(&value, 0, sizeof(value));
  size_t bytes = (app_id.size() + 1) * sizeof(char16_t);
  value.pwszVal = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
  if (value.pwszVal == nullptr) return E_OUTOFMEMORY;
  std::memcpy(value.pwszVal, app_id.c_str(), bytes);
  value.vt = VT_LPWSTR;
  hr = store->SetValue(kAppUserModelId, value);
  if (SUCCEEDED(hr)) hr = store->Commit();
  PropVariantClear(&value);
  return hr;
}

}  // namespace

std::u16string ProgramsFolder() {
  auto get = reinterpret_cast<SHGetKnownFolderPathFn>(Shell32("SHGetKnownFolderPath"));
  if (get == nullptr) return std::u16string();
  PWSTR path = nullptr;
  std::u16string result;
  if (SUCCEEDED(get(&kFolderIdPrograms, 0 /* KF_FLAG_DEFAULT */, nullptr, &path))) {
    result = U(path);
  }
  CoTaskMemFree(path);
  return result;
}

long CreateShortcut(const std::u16string& link_path, const std::u16string& target,
                    const std::u16string& arguments, const std::u16string& work_dir,
                    const std::u16string& icon, const std::u16string& description,
                    const std::u16string& app_id) {
  ComScope com;
  if (!com.ok()) return com.hr();
  Ref<IShellLinkW> link;
  HRESULT hr = CoCreateInstance(kClsidShellLink, nullptr, CLSCTX_INPROC_SERVER, kIidShellLinkW,
                                link.out());
  if (FAILED(hr)) return hr;
  hr = link->SetPath(W(target));
  if (SUCCEEDED(hr)) hr = link->SetArguments(W(arguments));
  if (SUCCEEDED(hr) && !work_dir.empty()) hr = link->SetWorkingDirectory(W(work_dir));
  if (SUCCEEDED(hr) && !icon.empty()) hr = link->SetIconLocation(W(icon), 0);
  if (SUCCEEDED(hr) && !description.empty()) hr = link->SetDescription(W(description));
  if (SUCCEEDED(hr) && !app_id.empty()) hr = SetAppId(link.get(), app_id);
  if (SUCCEEDED(hr)) {
    Ref<IPersistFile> file;
    hr = link->QueryInterface(kIidPersistFile, file.out());
    if (SUCCEEDED(hr)) hr = file->Save(W(link_path), TRUE);
  }
  if (SUCCEEDED(hr)) {
    if (auto notify = reinterpret_cast<SHChangeNotifyFn>(Shell32("SHChangeNotify"))) {
      notify(SHCNE_UPDATEITEM, SHCNF_PATHW, W(link_path), nullptr);
    }
  }
  return hr;
}

long SetProcessAppId(const std::u16string& app_id) {
  auto set = reinterpret_cast<SetAppIdFn>(Shell32("SetCurrentProcessExplicitAppUserModelID"));
  if (set == nullptr) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  return set(W(app_id));
}

std::u16string ProcessAppId() {
  auto get = reinterpret_cast<GetAppIdFn>(Shell32("GetCurrentProcessExplicitAppUserModelID"));
  if (get == nullptr) return std::u16string();
  PWSTR id = nullptr;
  std::u16string result;
  if (SUCCEEDED(get(&id))) result = U(id);
  CoTaskMemFree(id);
  return result;
}

bool MakeDirectories(const std::u16string& dir) {
  if (dir.empty()) return false;
  DWORD attributes = GetFileAttributesW(W(dir));
  if (attributes != INVALID_FILE_ATTRIBUTES) return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  size_t slash = dir.find_last_of(u"\\/");
  if (slash != std::u16string::npos && slash > 0) MakeDirectories(dir.substr(0, slash));
  return CreateDirectoryW(W(dir), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool RemoveFile(const std::u16string& path) {
  if (DeleteFileW(W(path))) return true;
  DWORD error = GetLastError();
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool RemoveEmptyDirectory(const std::u16string& dir) {
  if (RemoveDirectoryW(W(dir))) return true;
  DWORD error = GetLastError();
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool Exists(const std::u16string& path) {
  return GetFileAttributesW(W(path)) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace shell
}  // namespace gesher
