// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

namespace gesher::audio_playback {

// Hardware playback activity reported by a single registered process.
// A producer may only write its own region. No volume controls live here.
constexpr int kStreamCount = 12;
constexpr ULONGLONG kActiveMs = 50;
struct Activity {
  alignas(8) volatile LONG64 active_until_ms[kStreamCount];
};

}  // namespace gesher::audio_playback
