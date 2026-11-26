/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#pragma once

namespace gesher::store {

// Keep the explicit JNI wire values in sync with StoreEntitlement.java.
enum class Entitlement { kUnconfigured = 0, kUnavailable = 1, kInactive = 2, kActive = 3 };

bool EditionReleaseEnabled();
Entitlement ReadEntitlement();

}  // namespace gesher::store
