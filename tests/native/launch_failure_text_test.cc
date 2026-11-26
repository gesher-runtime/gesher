// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Run on Windows: these checks use the production helper and real Unicode NLS
// classification. They verify formatting, not a substitute renderer or dialog.
#include "../../compat/shell/launch_failure_text.h"

#include <cstdio>

static int checks;
static int failures;

static void Check(const char* label, const std::wstring& input,
                  const std::wstring& expected, bool rtl = true) {
  ++checks;
  if (GesherFormatLaunchFailureReason(input, rtl) != expected) {
    std::fprintf(stderr, "FAIL: %s\n", label);
    ++failures;
  }
}

int main() {
  Check("empty", L"", L"");
  Check("LTR dialogs unchanged", L"Error.\n\u05e9\u05dc\u05d5\u05dd", L"Error.\n\u05e9\u05dc\u05d5\u05dd", false);
  Check("single English paragraph", L"Cannot open app.apk (error 5).",
        L"\u202aCannot open app.apk (error 5).\u202c");
  Check("every LF paragraph", L"First failure.\nUse app.apk (x86_64).",
        L"\u202aFirst failure.\u202c\n\u202aUse app.apk (x86_64).\u202c");
  Check("CRLF and empty lines preserved", L"First failure.\r\n\r\nUse app.apk.\r\n",
        L"\u202aFirst failure.\u202c\r\n\r\n\u202aUse app.apk.\u202c\r\n");
  Check("Unicode paragraph separators", L"First.\u0085Second.\u2028Third.\u2029Fourth.",
        L"\u202aFirst.\u202c\u0085\u202aSecond.\u202c\u2028\u202aThird.\u202c\u2029\u202aFourth.\u202c");
  Check("C0 paragraph separators", L"First.\x001cSecond.\x001dThird.\x001e" L"Fourth.",
        L"\u202aFirst.\u202c\x001c\u202aSecond.\u202c\x001d\u202aThird.\u202c\x001e\u202aFourth.\u202c");
  Check("English with Hebrew path", L"Cannot open C:\\\u05e9\u05dc\u05d5\u05dd\\app.apk (error 5).",
        L"\u202aCannot open C:\\\u05e9\u05dc\u05d5\u05dd\\app.apk (error 5).\u202c");
  Check("Hebrew paragraph unchanged", L"\u05e9\u05d2\u05d9\u05d0\u05d4 (app.apk).",
        L"\u05e9\u05d2\u05d9\u05d0\u05d4 (app.apk).");
  Check("Arabic paragraph unchanged", L"\u062e\u0637\u0623 (app.apk).",
        L"\u062e\u0637\u0623 (app.apk).");
  Check("mixed paragraph directions", L"\u05e9\u05d2\u05d9\u05d0\u05d4.\nError 5.\n\u05e9\u05dc\u05d5\u05dd.",
        L"\u05e9\u05d2\u05d9\u05d0\u05d4.\n\u202aError 5.\u202c\n\u05e9\u05dc\u05d5\u05dd.");
  Check("localized tokens preserved", L"\u05e9\u05d2\u05d9\u05d0\u05d4 \u202aapp.apk\u202c (x86_64).",
        L"\u05e9\u05d2\u05d9\u05d0\u05d4 \u202aapp.apk\u202c (x86_64).");
  Check("leading embedding preserved", L" \u202aError 5.\u202c\nNext.",
        L" \u202aError 5.\u202c\n\u202aNext.\u202c");
  Check("leading RTL embedding preserved", L"\u202bError 5.\u202c",
        L"\u202bError 5.\u202c");
  Check("leading isolate preserved", L"\u2068app.apk\u2069",
        L"\u2068app.apk\u2069");
  Check("nested isolate preserved", L"Error: \u2068\u05e9\u05dc\u05d5\u05dd.apk\u2069.",
        L"\u202aError: \u2068\u05e9\u05dc\u05d5\u05dd.apk\u2069.\u202c");
  Check("non-Latin LTR text", L"\u0391\u03c1\u03c7\u03b5\u03af\u03bf (app.apk).",
        L"\u202a\u0391\u03c1\u03c7\u03b5\u03af\u03bf (app.apk).\u202c");
  Check("emoji selector is not RTL", L"\u26a0\ufe0f Cannot open app.apk.",
        L"\u202a\u26a0\ufe0f Cannot open app.apk.\u202c");
  Check("leading punctuation", L"(Error 5).", L"\u202a(Error 5).\u202c");
  Check("numeric diagnostic", L"123 (456).", L"\u202a123 (456).\u202c");
  Check("neutral diagnostic", L"(?).", L"\u202a(?).\u202c");
  Check("whitespace untouched", L" \t\r\n\t\n", L" \t\r\n\t\n");
  std::printf("launch failure text: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
