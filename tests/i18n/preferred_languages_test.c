// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include "../../compat/locale/ui_locale.h"

#define PROFILE(value, type) mock_set_profile(value, sizeof(value), type)
#define DISPLAY(value) mock_set_display(value, sizeof(value) / sizeof(wchar_t))
static unsigned checks;

static void expect_names(const wchar_t* expected) {
  mock_allocations = 0;
  wchar_t* actual = GesherPreferredLanguages();
  if (expected == NULL) {
    assert(actual == NULL);
  } else {
    assert(actual != NULL);
    const wchar_t* name = actual;
    const wchar_t* want = expected;
    while (*want != L'\0') {
      const wchar_t* end = wcschr(want, L',');
      size_t length = end == NULL ? wcslen(want) : (size_t)(end - want);
      assert(wcslen(name) == length);
      assert(wcsncmp(name, want, length) == 0);
      name += length + 1;
      want += length;
      if (*want == L',') ++want;
    }
    assert(*name == L'\0');
    HeapFree(GetProcessHeap(), 0, actual);
  }
  assert(mock_live_allocations == 0 && mock_open_keys == 0);
  ++checks;
}
static void expect_language(enum GesherUiLanguage expected) {
  mock_allocations = 0;
  assert(GesherUiCurrentLanguage() == expected);
  assert(mock_live_allocations == 0 && mock_open_keys == 0);
  ++checks;
}
static void expect_rtl(BOOL expected) {
  assert(GesherDesktopRtl() == expected);
  assert(mock_live_allocations == 0 && mock_open_keys == 0);
  ++checks;
}

int main(void) {
  mock_reset();
  PROFILE(L"he-IL\0en-US\0", REG_MULTI_SZ); DISPLAY(L"en-US\0");
  expect_names(L"he-IL,en-US"); expect_language(GESHER_UI_HE);
  mock_reset();
  PROFILE(L"en-US\0he-IL\0", REG_MULTI_SZ); DISPLAY(L"he-IL\0");
  expect_names(L"en-US,he-IL"); expect_language(GESHER_UI_EN);
  mock_reset();
  PROFILE(L"fr-FR\0", REG_MULTI_SZ); DISPLAY(L"he-IL\0");
  expect_names(L"fr-FR,he-IL"); expect_language(GESHER_UI_HE);
  mock_reset();
  PROFILE(L"\0", REG_MULTI_SZ); DISPLAY(L"he-IL\0");
  expect_names(L"he-IL"); expect_language(GESHER_UI_HE);
  mock_reset();
  PROFILE(L"he-IL", REG_SZ); DISPLAY(L"en-US\0");
  expect_names(L"he-IL,en-US"); expect_language(GESHER_UI_HE);
  mock_reset();
  PROFILE(L"fr-FR\0he-IL\0", REG_SZ); DISPLAY(L"en-US\0");
  expect_names(L"fr-FR,en-US"); expect_language(GESHER_UI_EN);
  mock_reset();
  PROFILE(L"fr-FR\0\0he-IL\0", REG_MULTI_SZ); DISPLAY(L"en-US\0");
  expect_names(L"fr-FR,en-US"); expect_language(GESHER_UI_EN);
  mock_reset();
  PROFILE(L"he-IL\0HE-il\0en-US\0", REG_MULTI_SZ); DISPLAY(L"EN-us\0fr-FR\0\0he\0");
  expect_names(L"he-IL,en-US,fr-FR"); expect_language(GESHER_UI_HE);
  mock_reset();
  PROFILE(L" \the-IL\r\n\0bad,tag\0fr--FR\0en-US\0", REG_MULTI_SZ); DISPLAY(L"en-US\0");
  expect_names(L"he-IL,en-US"); expect_language(GESHER_UI_HE);

  // Unterminated string data is bounded by the reported byte count.
  const wchar_t unterminated[] = {L'h', L'e', L'-', L'I', L'L'};
  mock_reset();
  mock_set_profile(unterminated, sizeof(unterminated), REG_MULTI_SZ); DISPLAY(L"en-US\0");
  expect_names(L"he-IL,en-US"); expect_language(GESHER_UI_HE);
  mock_reset();
  mock_set_display(unterminated, sizeof(unterminated) / sizeof(wchar_t));
  expect_names(L"he-IL"); expect_language(GESHER_UI_HE);

  // Reject malformed or changed registry metadata on either query.
  mock_reset();
  PROFILE(L"he-IL", REG_DWORD); DISPLAY(L"en-US\0");
  expect_names(L"en-US"); expect_language(GESHER_UI_EN);
  mock_reset();
  PROFILE(L"he-IL", REG_SZ); DISPLAY(L"en-US\0");
  mock_profile_probe_bytes--;
  expect_names(L"en-US"); expect_language(GESHER_UI_EN);
  mock_reset();
  PROFILE(L"he-IL", REG_SZ); DISPLAY(L"en-US\0");
  mock_profile_return_bytes--;
  expect_names(L"en-US"); expect_language(GESHER_UI_EN);
  mock_reset();
  PROFILE(L"he-IL", REG_SZ); DISPLAY(L"en-US\0");
  mock_profile_return_type = REG_DWORD;
  expect_names(L"en-US"); expect_language(GESHER_UI_EN);
  mock_reset();
  PROFILE(L"he-IL", REG_SZ); DISPLAY(L"en-US\0");
  mock_profile_return_bytes += sizeof(wchar_t);
  expect_names(L"en-US"); expect_language(GESHER_UI_EN);
  mock_reset();
  PROFILE(L"he-IL", REG_SZ); DISPLAY(L"en-US\0");
  mock_profile_read_status = ERROR_MORE_DATA;
  expect_names(L"en-US"); expect_language(GESHER_UI_EN);
  mock_reset();
  PROFILE(L"he-IL", REG_SZ); DISPLAY(L"en-US\0");
  mock_display_return_chars++;
  expect_names(L"he-IL"); expect_language(GESHER_UI_HE);
  mock_reset();
  PROFILE(L"he-IL", REG_SZ); DISPLAY(L"en-US\0");
  mock_display_read_success = 0;
  expect_names(L"he-IL"); expect_language(GESHER_UI_HE);
  mock_reset();
  expect_names(NULL); expect_language(GESHER_UI_EN);

  // Each independently allocated source can fail without discarding the other.
  mock_reset();
  PROFILE(L"he-IL", REG_SZ); DISPLAY(L"en-US\0");
  mock_fail_allocation = 1;
  expect_names(L"en-US"); expect_language(GESHER_UI_EN);
  mock_reset();
  PROFILE(L"he-IL", REG_SZ); DISPLAY(L"en-US\0");
  mock_fail_allocation = 2;
  expect_names(L"he-IL"); expect_language(GESHER_UI_HE);
  mock_reset();
  PROFILE(L"he-IL", REG_SZ); DISPLAY(L"en-US\0");
  mock_fail_allocation = 3;
  expect_names(NULL); expect_language(GESHER_UI_EN);

  // The UI selector must reach supported names after >1024 characters.
  wchar_t long_profile[8192];
  size_t used = 0;
  for (unsigned i = 0; i < 300; ++i) {
    int length = swprintf(long_profile + used, 8192 - used, L"qaa-x-%04u", i);
    assert(length > 0);
    used += (size_t)length + 1;
  }
  memcpy(long_profile + used, L"he-IL\0", sizeof(L"he-IL\0"));
  used += sizeof(L"he-IL\0") / sizeof(wchar_t);
  assert(used > 1024);
  mock_reset();
  mock_set_profile(long_profile, used * sizeof(wchar_t), REG_MULTI_SZ); DISPLAY(L"en-US\0");
  expect_language(GESHER_UI_HE);

  // Explicit launch preferences still take precedence, even when unsupported.
  mock_environment = L"en-US,he-IL"; expect_language(GESHER_UI_EN);
  mock_environment = L"he-IL,en-US"; expect_language(GESHER_UI_HE);
  mock_environment = L"fr-FR"; expect_language(GESHER_UI_EN);
  mock_environment = L" \t\r\n"; expect_language(GESHER_UI_HE);

  // Layout is determined by Windows even when Gesher has no matching translation.
  mock_reset();
  PROFILE(L"ar-EG\0en-US\0", REG_MULTI_SZ);
  mock_layout_locale = L"ar-EG"; mock_layout = 1;
  expect_language(GESHER_UI_EN); expect_rtl(1);
  mock_environment = L" \tar_EG,en-US"; expect_rtl(1);
  mock_environment = L"en-US,ar-EG"; expect_rtl(0);
  mock_reset();
  PROFILE(L"he-IL\0", REG_MULTI_SZ);
  expect_rtl(1); // Unknown Windows reading layout falls back to the matching UI language.
  mock_layout_locale = L"he-IL"; mock_layout = 2;
  expect_rtl(0); // A vertical layout is not horizontal RTL.
  mock_reset();
  printf("PASS: %u preferred-language boundary and selector checks\n", checks);
  return 0;
}
