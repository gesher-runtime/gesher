// Gesher: the WebView2 loader (docs/webview.md, "The loader").
//
// GESHER_WEBVIEW2_STATIC: WebView2LoaderStatic.lib from the SDK is linked in
// (it is built with MSVC; msvc_shims.cc supplies the few pieces of MSVC's
// runtime it uses). Otherwise WebView2Loader.dll is loaded at run time from
// the executable's directory, so an executable without it still runs and
// WebView reports that WebView2 is unavailable.

#include "webview2_sdk.h"

#ifdef GESHER_WEBVIEW2_STATIC

namespace gesher::webview {

HRESULT CreateEnvironment(PCWSTR browser_folder, PCWSTR user_data_folder,
                          ICoreWebView2EnvironmentOptions* options,
                          ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler) {
  return ::CreateCoreWebView2EnvironmentWithOptions(browser_folder, user_data_folder, options,
                                                    handler);
}

HRESULT AvailableBrowserVersion(LPWSTR* version) {
  return ::GetAvailableCoreWebView2BrowserVersionString(nullptr, version);
}

const char* LoaderKind() { return "static (WebView2LoaderStatic.lib)"; }

}  // namespace gesher::webview

#else  // WebView2Loader.dll

#include <mutex>
#include <string>

namespace gesher::webview {

namespace {

using CreateFn = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                          ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
using VersionFn = HRESULT(STDAPICALLTYPE*)(PCWSTR, LPWSTR*);

std::once_flag g_once;
CreateFn g_create = nullptr;
VersionFn g_version = nullptr;
bool g_found = false;

void Load() {
  std::call_once(g_once, [] {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dll;
    if (n > 0 && n < MAX_PATH) {
      dll.assign(path, n);
      size_t slash = dll.find_last_of(L"\\/");
      dll = (slash == std::wstring::npos ? std::wstring() : dll.substr(0, slash + 1)) +
            L"WebView2Loader.dll";
    }
    HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) return;
    g_found = true;
    g_create = reinterpret_cast<CreateFn>(
        reinterpret_cast<void*>(GetProcAddress(module, "CreateCoreWebView2EnvironmentWithOptions")));
    g_version = reinterpret_cast<VersionFn>(reinterpret_cast<void*>(
        GetProcAddress(module, "GetAvailableCoreWebView2BrowserVersionString")));
  });
}

}  // namespace

HRESULT CreateEnvironment(PCWSTR browser_folder, PCWSTR user_data_folder,
                          ICoreWebView2EnvironmentOptions* options,
                          ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler) {
  Load();
  if (g_create == nullptr) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
  return g_create(browser_folder, user_data_folder, options, handler);
}

HRESULT AvailableBrowserVersion(LPWSTR* version) {
  Load();
  if (g_version == nullptr) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
  return g_version(nullptr, version);
}

const char* LoaderKind() {
  Load();
  return g_found ? "WebView2Loader.dll" : "WebView2Loader.dll (missing)";
}

}  // namespace gesher::webview

#endif
