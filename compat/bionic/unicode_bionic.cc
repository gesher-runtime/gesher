// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "unicode_bionic.h"

#include <unicode/uchar.h>

namespace gesher::bionic {

uint32_t UnicodeValue(uint32_t c, UnicodeProperty property) {
  // Bionic deliberately uses byte C-locale rules below 0x80. In particular,
  // vertical tab is whitespace and U+001C is not, even if ICU says otherwise.
  if (c < 128) {
    bool upper = c >= 'A' && c <= 'Z';
    bool lower = c >= 'a' && c <= 'z';
    bool digit = c >= '0' && c <= '9';
    switch (property) {
      case UnicodeProperty::Alnum:
        return upper || lower || digit;
      case UnicodeProperty::Alpha:
        return upper || lower;
      case UnicodeProperty::Blank:
        return c == ' ' || c == '\t';
      case UnicodeProperty::Cntrl:
        return c < 32 || c == 127;
      case UnicodeProperty::Digit:
        return digit;
      case UnicodeProperty::Graph:
        return c > 32 && c < 127;
      case UnicodeProperty::Lower:
        return lower;
      case UnicodeProperty::Print:
        return c >= 32 && c < 127;
      case UnicodeProperty::Punct:
        return c > 32 && c < 127 && !(upper || lower || digit);
      case UnicodeProperty::Space:
        return c == ' ' || (c >= 9 && c <= 13);
      case UnicodeProperty::Upper:
        return upper;
      case UnicodeProperty::Xdigit:
        return digit || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
      case UnicodeProperty::ToLower:
        return upper ? c + ('a' - 'A') : c;
      case UnicodeProperty::ToUpper:
        return lower ? c - ('a' - 'A') : c;
    }
  }
  if (property == UnicodeProperty::ToLower ||
      property == UnicodeProperty::ToUpper) {
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return c;
    return property == UnicodeProperty::ToLower ? u_tolower(c) : u_toupper(c);
  }
  if (c > 0x10ffff) return 0;
  switch (property) {
    case UnicodeProperty::Alnum:
      return u_hasBinaryProperty(c, UCHAR_POSIX_ALNUM);
    case UnicodeProperty::Alpha:
      return u_hasBinaryProperty(c, UCHAR_ALPHABETIC);
    case UnicodeProperty::Blank:
      return u_hasBinaryProperty(c, UCHAR_POSIX_BLANK);
    case UnicodeProperty::Cntrl:
      return u_charType(c) == U_CONTROL_CHAR;
    case UnicodeProperty::Digit:
      return u_charType(c) == U_DECIMAL_DIGIT_NUMBER;
    case UnicodeProperty::Graph:
      return u_hasBinaryProperty(c, UCHAR_POSIX_GRAPH);
    case UnicodeProperty::Lower:
      return u_hasBinaryProperty(c, UCHAR_LOWERCASE);
    case UnicodeProperty::Print:
      return u_hasBinaryProperty(c, UCHAR_POSIX_PRINT);
    case UnicodeProperty::Punct: {
      // The pinned wctype.cpp uses these bounds, excluding Pi/Pf categories.
      int category = u_charType(c);
      return category >= U_DASH_PUNCTUATION && category <= U_OTHER_PUNCTUATION;
    }
    case UnicodeProperty::Space:
      return u_hasBinaryProperty(c, UCHAR_WHITE_SPACE);
    case UnicodeProperty::Upper:
      return u_hasBinaryProperty(c, UCHAR_UPPERCASE);
    case UnicodeProperty::Xdigit:
      return u_hasBinaryProperty(c, UCHAR_POSIX_XDIGIT);
    default:
      return 0;
  }
}

}  // namespace gesher::bionic
