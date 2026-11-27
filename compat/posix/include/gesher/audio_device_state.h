// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <cstdint>

namespace gesher::audio_state {

// Hardware playback activity reported by a single registered process.
// A producer may only write its own region. No volume controls live here.
constexpr int kStreamCount = 12;
constexpr ULONGLONG kActiveMs = 50;
struct Activity {
  alignas(8) volatile LONG64 active_until_ms[kStreamCount];
};

// Shared Android device controls. On a protected device the broker only
// duplicates a writable handle into the authenticated SystemServer. The
// layout also remains compatible with the existing legacy named mapping.
struct DeviceState {
  uint32_t magic;
  uint32_t version;
  volatile LONG generation;
  volatile LONG next_session;
  volatile LONG lock;
  int32_t index[kStreamCount];
  int32_t min_index[kStreamCount];
  int32_t max_index[kStreamCount];
  int32_t muted[kStreamCount];
  volatile LONG64 active_until_ms[kStreamCount];
  float master_volume;
  int32_t master_mute;
};

constexpr uint32_t kMagic = 0x47415544;  // 'GAUD'
constexpr uint32_t kVersion = 1;

// AudioService's default limits. Windows sessions start at full volume.
inline void InitDeviceState(DeviceState* state) {
  *state = {};
  constexpr int kMaxIndex[kStreamCount] = {5, 7, 7, 15, 7, 7, 15, 7, 15, 15, 15, 15};
  constexpr int kMinIndex[kStreamCount] = {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};
  state->version = kVersion;
  state->next_session = 1;
  for (int i = 0; i < kStreamCount; ++i) {
    state->min_index[i] = kMinIndex[i];
    state->max_index[i] = kMaxIndex[i];
    state->index[i] = kMaxIndex[i];
  }
  state->master_volume = 1.0f;
  MemoryBarrier();
  state->magic = kMagic;
}

}  // namespace gesher::audio_state
