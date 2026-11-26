// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace gesher {
// After configuring the native authority route, authenticate the exact process
// and adopt only the resources returned by that authority. Run before ART, an
// ELF program or shell code; no environment/guest handle tuple is accepted.
uint32_t InitializeProtectedProcessResources();
}  // namespace gesher
