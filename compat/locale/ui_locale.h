// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include "ui_messages.h"

// Same preference order as SystemLocale, including its explicit test/launch override.
// Read at presentation time so a later Windows language change is picked up.
static inline enum GesherUiLanguage GesherUiCurrentLanguage(void) {
  DWORD size = GetEnvironmentVariableW(L"GESHER_LOCALES", NULL, 0);
  if (size > 1) {
    wchar_t* tags = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, size * sizeof(wchar_t));
    if (tags != NULL) {
      DWORD copied = GetEnvironmentVariableW(L"GESHER_LOCALES", tags, size);
      if (copied > 0 && copied < size) {
        const wchar_t* value = tags;
        while (*value == L' ' || *value == L'\t' || *value == L'\r' || *value == L'\n') ++value;
        if (*value) {
          enum GesherUiLanguage lang = GesherUiMatch(value);
          HeapFree(GetProcessHeap(), 0, tags);
          return lang;
        }
      }
      HeapFree(GetProcessHeap(), 0, tags);
    }
  }
  ULONG count = 0, length = 0;
  if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, NULL, &length) && length > 1) {
    wchar_t* tags = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, length * sizeof(wchar_t));
    if (tags != NULL) {
      if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, tags, &length)) {
        // Convert MULTI_SZ to the comma-separated form used by all other consumers.
        for (ULONG i = 0; i + 1 < length; ++i) if (tags[i] == L'\0') tags[i] = L',';
        enum GesherUiLanguage lang = GesherUiMatch(tags);
        HeapFree(GetProcessHeap(), 0, tags);
        return lang;
      }
      HeapFree(GetProcessHeap(), 0, tags);
    }
  }
  return GESHER_UI_EN;
}
