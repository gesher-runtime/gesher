// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../../binder/protected_data_client.h"

namespace gesher {

// Native startup must opt in before any Binder or Unix rights operation.
// Invalid routing remains protected and fails closed. The actual pipe is
// authenticated separately when opening each connection.
DWORD ConfigureProtectedBinder(const binder::ProtectedDataEndpoint& endpoint);

// The first read seals the configuration for the process lifetime. Null
// denotes ordinary mode. A non-null endpoint may contain invalid routing.
const binder::ProtectedDataEndpoint* ProtectedBinderEndpoint();

// Open the authenticated private connection, or return INVALID_HANDLE_VALUE.
HANDLE OpenProtectedBinder(const binder::ProtectedDataEndpoint& endpoint);

}  // namespace gesher
