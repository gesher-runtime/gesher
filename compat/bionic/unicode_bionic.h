// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

namespace gesher::bionic {
enum class UnicodeProperty {
  Alnum,
  Alpha,
  Blank,
  Cntrl,
  Digit,
  Graph,
  Lower,
  Print,
  Punct,
  Space,
  Upper,
  Xdigit,
  ToLower,
  ToUpper
};

uint32_t UnicodeValue(uint32_t codepoint, UnicodeProperty property);
}  // namespace gesher::bionic
