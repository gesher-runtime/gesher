// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

// Crash arguments are LTR identifiers: an Android package name, an absolute
// Windows report path (possibly containing RTL directory names), and an event
// ID. The shared catalog uses Unicode isolates, which classic Win32 STATIC and
// EDIT controls render as missing glyphs. Use the legacy LTR embedding those
// controls support, as for launch-failure diagnostics. Translate only catalog
// markers: argument contents, including braces, remain literal.
static inline std::wstring GesherFormatAppCrashText(const std::wstring& pattern,
                                                   const wchar_t* package,
                                                   const wchar_t* report,
                                                   const wchar_t* event) {
  std::wstring message;
  for (size_t i = 0; i < pattern.size();) {
    if (pattern.compare(i, 3, L"{0}") == 0) { message += package; i += 3; }
    else if (pattern.compare(i, 3, L"{1}") == 0) { message += report; i += 3; }
    else if (pattern.compare(i, 3, L"{2}") == 0) { message += event; i += 3; }
    else {
      wchar_t c = pattern[i++];
      if (c == 0x2068) c = 0x202a;  // FSI -> LRE
      else if (c == 0x2069) c = 0x202c;  // PDI -> PDF
      message += c;
    }
  }
  return message;
}
