// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Windows host test of compat/devsvc/url_handlers_win.cc (nothing is launched):
// a scheme is "registered" exactly when Windows resolves a protocol handler for it.
// It registers two throwaway per-user schemes under HKCU\Software\Classes (one with an
// open command, one with only "URL Protocol"), asks, and removes them. Then it prints
// the runner's own answers for mailto/tel/sms/geo/http, which are not asserted.
// Real Gesher confirmation dialogs are inspected and automatically dismissed by
// a thread-local CBT hook. No phone app or tel URI is opened.
//
//   cl /nologo /std:c++17 /EHsc /W4 /WX compat\devsvc\url_handlers_win.cc
//      tests\uri-handlers\url_handlers_windows_test.cc /Fe:url-handlers-test.exe
//      shlwapi.lib shell32.lib advapi32.lib user32.lib
#include "../../compat/devsvc/url_handlers_win.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cwchar>
#include <initializer_list>
#include <string>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* what) {
  ++g_checks;
  if (!condition) ++g_failures;
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
}

const char* Name(dev::UrlHandler h) {
  switch (h) {
    case dev::UrlHandler::kRegistered: return "registered";
    case dev::UrlHandler::kNotRegistered: return "not registered";
    case dev::UrlHandler::kInvalidScheme: return "invalid scheme";
  }
  return "?";
}

void Expect(const wchar_t* scheme, dev::UrlHandler expected, const char* what) {
  long hr = 0;
  dev::UrlHandler actual = dev::QueryUrlHandler(scheme, &hr);
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    std::printf("FAIL %s: %ls is %s (hr 0x%08lx), expected %s\n", what, scheme, Name(actual),
                static_cast<unsigned long>(hr), Name(expected));
  } else {
    std::printf("ok   %s: %ls is %s\n", what, scheme, Name(actual));
  }
}

bool SetValue(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS) {
    return false;
  }
  LONG r = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return r == ERROR_SUCCESS;
}

// Tells the shell that associations changed, as an installer does.
void Notify() { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); }

struct DialogTest {
  bool active = true;
  bool observed = false;
  int choice = IDCANCEL;  // zero: invalidate the request and let its real timer cancel.
  HWND owner = nullptr;
};
DialogTest* dialog_test = nullptr;

bool IsActive(void* context) { return static_cast<DialogTest*>(context)->active; }

LRESULT CALLBACK DialogHook(int code, WPARAM parameter, LPARAM extra) {
  if (code == HCBT_ACTIVATE && dialog_test) {
    HWND window = reinterpret_cast<HWND>(parameter);
    wchar_t title[64]{};
    GetWindowTextW(window, title, 64);
    if (std::wcscmp(title, L"Gesher - Confirm dialing") == 0) {
      DialogTest& test = *dialog_test;
      test.observed = true;
      Check(LOWORD(SendMessageW(window, DM_GETDEFID, 0, 0)) == IDCANCEL, "Cancel is the default button");
      wchar_t message[2048]{};
      GetWindowTextW(GetDlgItem(window, 0xFFFF), message, 2048);
      Check(std::wcsstr(message, L"+15551234;ext=9") != nullptr, "dialog displays the actual number and extension");
      Check(std::wcsstr(message, L"com.gesher.fixture") != nullptr, "dialog identifies the requesting package");
      Check(std::wcsstr(message, L"may place the call immediately") != nullptr,
            "dialog explains the actual shell handoff");
      if (test.owner) {
        Check(GetWindow(window, GW_OWNER) == test.owner, "confirmation belongs to the caller window");
        Check(!IsWindowEnabled(test.owner), "caller window is disabled while confirmation is modal");
      }
      if (test.choice) PostMessageW(window, WM_COMMAND, static_cast<WPARAM>(test.choice), 0);
      else test.active = false;
    }
  }
  return CallNextHookEx(nullptr, code, parameter, extra);
}

void ConfirmationContracts() {
  DialogTest expired;
  expired.active = false;
  Check(!dev::ConfirmDial(L"com.gesher.fixture", L"+15551234;ext=9", 0, IsActive, &expired),
        "retired request opens no dialog");
  Check(!dev::ConfirmDial(L"com.gesher.fixture", L"", 0, IsActive, &expired), "empty phone number fails closed");
  HHOOK hook = SetWindowsHookExW(WH_CBT, DialogHook, nullptr, GetCurrentThreadId());
  Check(hook != nullptr, "dialog inspection hook installed");
  if (!hook) return;
  HWND owner = CreateWindowExW(0, L"STATIC", L"Gesher dial fixture owner", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
      CW_USEDEFAULT, CW_USEDEFAULT, 320, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  Check(owner != nullptr, "caller window created");
  for (int choice : {IDCANCEL, IDOK, 0}) {
    DialogTest test;
    test.choice = choice;
    test.owner = owner;
    dialog_test = &test;
    bool result = dev::ConfirmDial(L"com.gesher.fixture", L"+15551234;ext=9",
        owner ? GetCurrentProcessId() : 0, IsActive, &test);
    dialog_test = nullptr;
    Check(test.observed, "real confirmation dialog was inspected");
    Check(result == (choice == IDOK), choice == 0 ? "timer cancels a retired live dialog"
        : choice == IDOK ? "explicit OK accepts review" : "Cancel declines review");
    if (owner) Check(IsWindowEnabled(owner), "caller window is enabled after confirmation");
  }
  if (owner) DestroyWindow(owner);
  UnhookWindowsHookEx(hook);
}

}  // namespace

int main() {
  // Not schemes (RFC 3986 section 3.1): nothing is asked of Windows.
  Expect(L"", dev::UrlHandler::kInvalidScheme, "empty");
  Expect(L"1tel", dev::UrlHandler::kInvalidScheme, "leading digit");
  Expect(L"mail to", dev::UrlHandler::kInvalidScheme, "space");
  Expect(L"mailto:", dev::UrlHandler::kInvalidScheme, "colon");
  Expect(L"..\\mailto", dev::UrlHandler::kInvalidScheme, "path");
  Expect(std::wstring(80, L'a').c_str(), dev::UrlHandler::kInvalidScheme, "too long");

  wchar_t name[64];
  std::swprintf(name, 64, L"gesher-test-%lu", static_cast<unsigned long>(GetCurrentProcessId()));
  std::wstring with_command = std::wstring(name) + L"-cmd";
  std::wstring protocol_only = std::wstring(name) + L"-bare";
  const std::wstring classes = L"Software\\Classes\\";

  Expect(with_command.c_str(), dev::UrlHandler::kNotRegistered, "unregistered scheme");

  wchar_t system32[MAX_PATH];
  UINT n = GetSystemDirectoryW(system32, MAX_PATH);
  std::wstring command = L"\"" + std::wstring(system32, n) + L"\\cmd.exe\" /c exit \"%1\"";
  bool created = SetValue(HKEY_CURRENT_USER, classes + with_command, L"URL Protocol", L"") &&
                 SetValue(HKEY_CURRENT_USER, classes + with_command + L"\\shell\\open\\command",
                          nullptr, command) &&
                 SetValue(HKEY_CURRENT_USER, classes + protocol_only, L"URL Protocol", L"");
  ++g_checks;
  if (!created) {
    ++g_failures;
    std::printf("FAIL could not create the per-user test protocols\n");
  } else {
    Notify();
    Expect(with_command.c_str(), dev::UrlHandler::kRegistered, "per-user protocol with an open command");
    Expect(protocol_only.c_str(), dev::UrlHandler::kNotRegistered, "\"URL Protocol\" without a handler");
  }
  RegDeleteTreeW(HKEY_CURRENT_USER, (classes + with_command).c_str());
  RegDeleteTreeW(HKEY_CURRENT_USER, (classes + protocol_only).c_str());
  Notify();
  Expect(with_command.c_str(), dev::UrlHandler::kNotRegistered, "after removal");

  // This machine's own associations: reported, not asserted.
  for (const wchar_t* scheme : {L"mailto", L"tel", L"sms", L"geo", L"http"}) {
    long hr = 0;
    dev::UrlHandler h = dev::QueryUrlHandler(scheme, &hr);
    std::printf("info %ls: %s (hr 0x%08lx)\n", scheme, Name(h), static_cast<unsigned long>(hr));
  }

  ConfirmationContracts();

  std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "url-handlers PASSED" : "url-handlers FAILED",
              g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
