// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>

namespace gesher::bionic {

// C FLT_ROUNDS/gdtoa values, independent of host FE_* constants:
// zero, nearest/even, upward, downward.
int Binary128RoundingMode();

// Writes the little-endian IEEE binary128 representation to 16 output bytes.
// The result is never converted to a host long double or double. An ABI
// adapter can pass the guest's rounding mode explicitly.
void ParseBinary128(const char* text, char** end, int rounding, void* output);

// Bionic wcstold commits a parsefloat grammar prefix before calling strtold.
// In particular, malformed NaN payloads and invalid signed input must be
// trimmed before gdtoa sees them, not merely fixed up in the end pointer.
size_t Binary128WidePrefix(const char* text);

}  // namespace gesher::bionic
