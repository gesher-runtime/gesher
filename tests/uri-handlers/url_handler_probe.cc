// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Manual probe (tests/uri-handlers/HUMAN_TEST.md): what Windows reports for each scheme
// Gesher hands to it, with the app Windows would run. Launches nothing unless --open URI
// is given, which runs ShellExecuteExW exactly as WindowsHost.openUrl does.
//
//   cl /nologo /std:c++17 /EHsc /W4 /WX compat\devsvc\url_handlers_win.cc
//      tests\uri-handlers\url_handler_probe.cc /Fe:url-handler-probe.exe
//      shlwapi.lib shell32.lib user32.lib advapi32.lib
#include "../../compat/devsvc/url_handlers_win.h"

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <cstdio>
#include <initializer_list>
#include <cwchar>

namespace {

void Print(ASSOCSTR what, const char* label, const wchar_t* scheme) {
  wchar_t out[1024];
  DWORD size = 1024;
  HRESULT hr = AssocQueryStringW(ASSOCF_IS_PROTOCOL | ASSOCF_INIT_IGNOREUNKNOWN, what, scheme,
                                 L"open", out, &size);
  if (SUCCEEDED(hr)) {
    std::printf("    %s: %ls\n", label, out);
  } else {
    std::printf("    %s: (none, hr 0x%08lx)\n", label, static_cast<unsigned long>(hr));
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wcscmp(argv[1], L"--open") == 0) {
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.hwnd = GetForegroundWindow();
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = argv[2];
    sei.nShow = SW_SHOWNORMAL;
    BOOL ok = ShellExecuteExW(&sei);
    std::printf("ShellExecuteExW(%ls): %s (error %lu)\n", argv[2], ok ? "accepted" : "failed",
                ok ? 0UL : GetLastError());
    if (ok && sei.hProcess != nullptr) CloseHandle(sei.hProcess);
    return ok ? 0 : 1;
  }
  for (const wchar_t* scheme : {L"mailto", L"tel", L"sms", L"smsto", L"geo", L"http"}) {
    long hr = 0;
    dev::UrlHandler h = dev::QueryUrlHandler(scheme, &hr);
    std::printf("%ls: %s\n", scheme,
                h == dev::UrlHandler::kRegistered ? "registered"
                : h == dev::UrlHandler::kNotRegistered ? "not registered" : "invalid");
    Print(ASSOCSTR_FRIENDLYAPPNAME, "app", scheme);
    Print(ASSOCSTR_APPID, "AppUserModelID", scheme);
    Print(ASSOCSTR_COMMAND, "command", scheme);
    Print(ASSOCSTR_DELEGATEEXECUTE, "DelegateExecute", scheme);
  }
  return 0;
}
