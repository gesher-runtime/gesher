// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Modeled Win32 boundaries only; tests include the real production headers.
#pragma once
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
typedef uint32_t DWORD;
typedef uint32_t ULONG;
typedef int BOOL;
typedef void* HKEY;
typedef unsigned char* LPBYTE;
typedef wchar_t* LPWSTR;
#define HKEY_CURRENT_USER ((HKEY)1)
#define KEY_QUERY_VALUE 1
#define ERROR_SUCCESS 0
#define ERROR_MORE_DATA 234
#define REG_SZ 1
#define REG_DWORD 4
#define REG_MULTI_SZ 7
#define MUI_LANGUAGE_NAME 8
#define LOCALE_IREADINGLAYOUT 0x70
#define LOCALE_RETURN_NUMBER 0x20000000
#if !defined(_WIN32)
#define _wcsnicmp wcsncasecmp
#endif

static const wchar_t* mock_profile;
static DWORD mock_profile_bytes, mock_profile_probe_bytes, mock_profile_return_bytes;
static DWORD mock_profile_type, mock_profile_return_type;
static long mock_profile_read_status;
static const wchar_t* mock_display;
static ULONG mock_display_chars, mock_display_probe_chars, mock_display_return_chars;
static int mock_display_read_success;
static const wchar_t* mock_environment;
static const wchar_t* mock_layout_locale;
static DWORD mock_layout;
static unsigned mock_allocations, mock_live_allocations, mock_fail_allocation, mock_open_keys;

static void mock_reset(void) {
  assert(mock_live_allocations == 0 && mock_open_keys == 0);
  mock_profile = NULL;
  mock_profile_bytes = mock_profile_probe_bytes = mock_profile_return_bytes = 0;
  mock_profile_type = mock_profile_return_type = REG_MULTI_SZ;
  mock_profile_read_status = ERROR_SUCCESS;
  mock_display = NULL;
  mock_display_chars = mock_display_probe_chars = mock_display_return_chars = 0;
  mock_display_read_success = 1;
  mock_environment = NULL;
  mock_layout_locale = NULL;
  mock_layout = 0;
  mock_allocations = mock_fail_allocation = 0;
}
static void mock_set_profile(const wchar_t* value, size_t bytes, DWORD type) {
  mock_profile = value;
  mock_profile_bytes = mock_profile_probe_bytes = mock_profile_return_bytes = (DWORD)bytes;
  mock_profile_type = mock_profile_return_type = type;
}
static void mock_set_display(const wchar_t* value, size_t chars) {
  mock_display = value;
  mock_display_chars = mock_display_probe_chars = mock_display_return_chars = (ULONG)chars;
}
static DWORD GetEnvironmentVariableW(const wchar_t* name, wchar_t* buffer, DWORD capacity) {
  (void)name;
  if (mock_environment == NULL) return 0;
  DWORD chars = (DWORD)wcslen(mock_environment);
  if (buffer == NULL || capacity <= chars) return chars + 1;
  memcpy(buffer, mock_environment, (chars + 1) * sizeof(wchar_t));
  return chars;
}
static int GetLocaleInfoEx(const wchar_t* name, DWORD kind, LPWSTR buffer, int capacity) {
  assert(kind == (LOCALE_IREADINGLAYOUT | LOCALE_RETURN_NUMBER));
  if (!name || !mock_layout_locale || wcscmp(name, mock_layout_locale) != 0) return 0;
  assert((size_t)capacity * sizeof(wchar_t) >= sizeof(mock_layout));
  memcpy(buffer, &mock_layout, sizeof(mock_layout));
  return capacity;
}
static void* GetProcessHeap(void) { return NULL; }
static void* HeapAlloc(void* heap, DWORD flags, size_t size) {
  (void)heap; (void)flags;
  ++mock_allocations;
  if (mock_allocations == mock_fail_allocation || size > 1024 * 1024) return NULL;
  void* result = malloc(size);
  assert(result != NULL);
  memset(result, 0xa5, size);
  ++mock_live_allocations;
  return result;
}
static int HeapFree(void* heap, DWORD flags, void* allocation) {
  (void)heap; (void)flags;
  assert(allocation != NULL && mock_live_allocations != 0);
  free(allocation);
  --mock_live_allocations;
  return 1;
}
static long RegOpenKeyExW(HKEY root, const wchar_t* path, DWORD options, DWORD access, HKEY* key) {
  (void)root; (void)path; (void)options; (void)access;
  if (mock_profile == NULL) return 2;
  *key = (HKEY)2;
  ++mock_open_keys;
  return ERROR_SUCCESS;
}
static long RegQueryValueExW(HKEY key, const wchar_t* name, void* reserved,
                             DWORD* type, LPBYTE buffer, DWORD* bytes) {
  (void)key; (void)name; (void)reserved;
  if (buffer == NULL) {
    *type = mock_profile_type;
    *bytes = mock_profile_probe_bytes;
    return ERROR_SUCCESS;
  }
  DWORD capacity = *bytes;
  *type = mock_profile_return_type;
  *bytes = mock_profile_return_bytes;
  if (mock_profile_read_status != ERROR_SUCCESS) return mock_profile_read_status;
  // Model even inconsistent successful replies without writing past the caller.
  DWORD copied = capacity < mock_profile_bytes ? capacity : mock_profile_bytes;
  if (copied > mock_profile_return_bytes) copied = mock_profile_return_bytes;
  memcpy(buffer, mock_profile, copied);
  return ERROR_SUCCESS;
}
static long RegCloseKey(HKEY key) {
  (void)key;
  assert(mock_open_keys != 0);
  --mock_open_keys;
  return ERROR_SUCCESS;
}
static int GetUserPreferredUILanguages(DWORD flags, ULONG* count,
                                       wchar_t* buffer, ULONG* chars) {
  (void)flags;
  *count = mock_display == NULL ? 0 : 1;
  if (mock_display == NULL) { *chars = 0; return 0; }
  if (buffer == NULL) { *chars = mock_display_probe_chars; return 1; }
  ULONG capacity = *chars;
  *chars = mock_display_return_chars;
  if (!mock_display_read_success) return 0;
  ULONG copied = capacity < mock_display_chars ? capacity : mock_display_chars;
  if (copied > mock_display_return_chars) copied = mock_display_return_chars;
  memcpy(buffer, mock_display, (size_t)copied * sizeof(wchar_t));
  return 1;
}
