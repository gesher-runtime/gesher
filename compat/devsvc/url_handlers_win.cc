// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "url_handlers_win.h"

#include <windows.h>
#include <shlwapi.h>
#include <cwchar>
#include <string>

namespace dev {
namespace {

// RFC 3986 section 3.1: scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
bool ValidScheme(const wchar_t* scheme) {
  if (scheme == nullptr) return false;
  size_t n = 0;
  for (const wchar_t* p = scheme; *p != L'\0'; ++p, ++n) {
    wchar_t c = *p;
    bool alpha = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
    bool other = (c >= L'0' && c <= L'9') || c == L'+' || c == L'-' || c == L'.';
    if (n == 0 ? !alpha : !(alpha || other)) return false;
    if (n >= 64) return false;
  }
  return n > 0;
}

// S_FALSE with a non-empty size: the string exists (pszOut null asks for its size).
bool HasString(ASSOCSTR what, const wchar_t* scheme, long* hr_out) {
  DWORD size = 0;
  HRESULT hr = AssocQueryStringW(ASSOCF_IS_PROTOCOL | ASSOCF_INIT_IGNOREUNKNOWN, what, scheme,
                                 L"open", nullptr, &size);
  if (hr_out != nullptr) *hr_out = hr;
  return (hr == S_OK || hr == S_FALSE) && size > 1;
}

struct DialDialog {
  bool (*active)(void*);
  void* context;
};
thread_local DialDialog* current_dial = nullptr;
constexpr wchar_t kDialTitle[] = L"Gesher - Confirm dialing";

BOOL CALLBACK CancelDialWindow(HWND window, LPARAM) {
  wchar_t class_name[32]{};
  wchar_t title[64]{};
  GetClassNameW(window, class_name, 32);
  GetWindowTextW(window, title, 64);
  if (std::wcscmp(class_name, L"#32770") == 0 && std::wcscmp(title, kDialTitle) == 0) {
    PostMessageW(window, WM_COMMAND, IDCANCEL, 0);
  }
  return TRUE;
}

void CALLBACK DialLifetimeTimer(HWND, UINT, UINT_PTR, DWORD) {
  DialDialog* dialog = current_dial;
  if (dialog != nullptr && !dialog->active(dialog->context)) {
    EnumThreadWindows(GetCurrentThreadId(), CancelDialWindow, 0);
  }
}

struct DialOwner { DWORD pid; HWND window; };
BOOL CALLBACK FindDialOwner(HWND window, LPARAM parameter) {
  auto* owner = reinterpret_cast<DialOwner*>(parameter);
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid == owner->pid && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
    owner->window = window;
    return FALSE;
  }
  return TRUE;
}

}  // namespace

UrlHandler QueryUrlHandler(const wchar_t* scheme, long* last_error) {
  if (!ValidScheme(scheme)) {
    if (last_error != nullptr) *last_error = E_INVALIDARG;
    return UrlHandler::kInvalidScheme;
  }
  if (HasString(ASSOCSTR_COMMAND, scheme, last_error)) return UrlHandler::kRegistered;
  if (HasString(ASSOCSTR_DELEGATEEXECUTE, scheme, last_error)) return UrlHandler::kRegistered;
  return UrlHandler::kNotRegistered;
}

bool ConfirmDial(const wchar_t* package_name, const wchar_t* number, unsigned long owner_pid,
                 bool (*active)(void*), void* context) {
  if (!package_name || !number || !*number || !active || current_dial || !active(context)) return false;
  std::wstring message = L"Open the Windows phone app for this number?\n\n";
  message += number;
  message += L"\n\nRequested by: ";
  message += package_name;
  message += L"\n\nThe Windows app may place the call immediately.\nSelect OK to continue, or Cancel.";
  DialOwner owner{owner_pid, nullptr};
  if (owner_pid) EnumWindows(FindDialOwner, reinterpret_cast<LPARAM>(&owner));
  if (!active(context)) return false;
  DialDialog dialog{active, context};
  UINT_PTR timer = SetTimer(nullptr, 0, 100, DialLifetimeTimer);
  if (!timer) return false;
  current_dial = &dialog;
  int choice = MessageBoxW(owner.window, message.c_str(), kDialTitle,
                          MB_OKCANCEL | MB_DEFBUTTON2 | MB_ICONQUESTION | MB_SETFOREGROUND);
  current_dial = nullptr;
  KillTimer(nullptr, timer);
  return choice == IDOK && active(context);
}

}  // namespace dev
