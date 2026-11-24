// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "../../compat/posix/src/binder_transport.h"

// Intentionally link another complete static-library copy of the routing
// implementation into this DLL, as libandroid_runtime does in production.
extern "C" __declspec(dllexport)
const gesher::binder::ProtectedDataEndpoint* ReadProtectedBinderRoute() {
  return gesher::ProtectedBinderEndpoint();
}

extern "C" __declspec(dllexport) DWORD ReplaceProtectedBinderRoute() {
  return gesher::ConfigureProtectedBinder({});
}
