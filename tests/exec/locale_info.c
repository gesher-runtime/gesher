// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Actual Android ABI calls used by the separately built filesystem programs.
#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(test) do { if (!(test)) { fprintf(stderr, "locale info: %s failed\n", #test); ++failures; } } while (0)

int main(void) {
  CHECK(setlocale(LC_ALL, "C.UTF-8") != NULL);
  CHECK(strcmp(nl_langinfo(CODESET), "UTF-8") == 0);
  locale_t plain = newlocale(LC_CTYPE_MASK, "C", (locale_t)0);
  locale_t utf8 = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
  CHECK(plain != (locale_t)0 && utf8 != (locale_t)0);
  if (plain && utf8) {
    locale_t previous = uselocale(plain);
    CHECK(strcmp(nl_langinfo(CODESET), "US-ASCII") == 0);
    CHECK(strcmp(nl_langinfo_l(CODESET, utf8), "UTF-8") == 0);
    CHECK(strcmp(nl_langinfo(CODESET), "US-ASCII") == 0);
    uselocale(previous);
    CHECK(strcmp(nl_langinfo(CODESET), "UTF-8") == 0);
  }
  if (plain) freelocale(plain);
  if (utf8) freelocale(utf8);
  CHECK(strcmp(nl_langinfo(RADIXCHAR), ".") == 0);
  CHECK(strcmp(nl_langinfo(THOUSEP), "") == 0);
  CHECK(strcmp(nl_langinfo(DAY_1), "Sunday") == 0);
  CHECK(strcmp(nl_langinfo(ABMON_12), "Dec") == 0);
  errno = 0;
  CHECK(strcmp(nl_langinfo((nl_item)-1), "") == 0 && errno == 0);
  CHECK(setlocale(LC_ALL, "C") != NULL);
  CHECK(strcmp(nl_langinfo(CODESET), "US-ASCII") == 0);
  if (!failures) puts("Android locale information contracts passed");
  return failures != 0;
}
