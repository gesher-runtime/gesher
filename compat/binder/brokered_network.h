// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>
#include <cstdint>

namespace gesher::network_broker {
// Checks the real process token, never a guest-provided permission bit.
bool HasInternetCapabilities(HANDLE process);
}  // namespace gesher::network_broker
