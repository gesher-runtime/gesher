// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <climits>
#include <string>
#include <vector>

// Use Windows' Unicode direction classes rather than script ranges: an English
// error can contain a Hebrew path, and an emoji variation selector is not RTL.
// Preserve a paragraph which already declares its own direction.
static inline bool GesherLaunchFailureParagraphIsLtr(const std::wstring& text,
                                                     size_t start, size_t end) {
  const size_t length = end - start;
  if (length == 0 || length > static_cast<size_t>(INT_MAX)) return false;
  std::vector<WORD> types(length);
  if (!GetStringTypeW(CT_CTYPE2, text.data() + start, static_cast<int>(length),
                      types.data())) return false;
  bool content = false;
  for (size_t i = 0; i < length; ++i) {
    switch (text[start + i]) {
      case 0x202a: case 0x202b: case 0x202d: case 0x202e:  // LRE/RLE/LRO/RLO
      case 0x2066: case 0x2067: case 0x2068:               // LRI/RLI/FSI
        return false;
    }
    if (types[i] == C2_LEFTTORIGHT) return true;
    if (types[i] == C2_RIGHTTOLEFT) return false;
    if (types[i] != C2_WHITESPACE && types[i] != C2_SEGMENTSEPARATOR
        && types[i] != C2_BLOCKSEPARATOR) content = true;
  }
  // Numeric codes and punctuation have no strong character. Keep their order
  // too, without adding formatting to whitespace-only paragraphs.
  return content;
}

static inline std::wstring GesherFormatLaunchFailureReason(const std::wstring& reason,
                                                           bool rtl) {
  if (!rtl || reason.empty()) return reason;
  std::wstring formatted;
  formatted.reserve(reason.size());
  size_t start = 0;
  while (start < reason.size()) {
    size_t end = reason.find_first_of(L"\r\n\x001c\x001d\x001e\u0085\u2028\u2029", start);
    if (end == std::wstring::npos) end = reason.size();
    const bool ltr = GesherLaunchFailureParagraphIsLtr(reason, start, end);
    // An embedding ends at a paragraph separator. Close/reopen it for each
    // paragraph so later lines keep their punctuation and path order as well.
    if (ltr) formatted += L'\u202a';
    formatted.append(reason, start, end - start);
    if (ltr) formatted += L'\u202c';
    if (end < reason.size()) formatted += reason[end++];
    start = end;
  }
  return formatted;
}
