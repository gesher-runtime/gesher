// Gesher: the WebView2 SDK's header (Microsoft.Web.WebView2 from NuGet,
// pinned in cmake/webview2_sdk.cmake) with what it needs from Windows'
// headers, for llvm-mingw (docs/webview.md).
//
// Gesher compiles everything with NOGDI and WIN32_LEAN_AND_MEAN; WebView2.h
// needs COM's headers (objidl.h, oaidl.h) and its RECT/HWND types only, so
// they are included here explicitly.
#pragma once

#ifdef NOGDI
#undef NOGDI
#endif
#include <windows.h>
#include <objbase.h>
#include <oaidl.h>
#include <objidl.h>
#include <ole2.h>
#include <shlwapi.h>

#include "WebView2.h"

namespace gesher::webview {

// The loader's entry points: from WebView2LoaderStatic.lib linked in, or from
// WebView2Loader.dll next to the executable (webview2_loader.cc).
HRESULT CreateEnvironment(PCWSTR browser_folder, PCWSTR user_data_folder,
                          ICoreWebView2EnvironmentOptions* options,
                          ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler);
// The version of the WebView2 Runtime the loader would use ("141.0.3537.71"),
// CoTaskMemFree'd by the caller; an error when no runtime is installed.
HRESULT AvailableBrowserVersion(LPWSTR* version);
// How the loader is linked: "static" or "dll" (and whether the DLL was found).
const char* LoaderKind();

}  // namespace gesher::webview
