// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "url_handlers_win.h"

#include <windows.h>
#include <shlwapi.h>

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

}  // namespace dev
