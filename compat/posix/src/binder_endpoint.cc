// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "../include/gesher/binder_endpoint.h"

#if defined(GESHER) && !defined(GESHER_PROTECTED_BINDER)
#include <gesher/fork.h>
#endif

namespace gesher::binder_endpoint {
namespace {
constinit Binding g_binding;
}  // namespace

extern "C" void gesher_binder_endpoint_anchor_v1() {}
extern "C" __declspec(dllexport) Binding* gesher_binder_endpoint_owner_v1() {
  return &g_binding;
}

// Each executable owns the inherited broker pin and the lock guarding it.
// An Android DLL calling Binder resolves this same owner in the executable.
Binding& Current() {
  using Factory = Binding* (*)();
  const auto factory = reinterpret_cast<Factory>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(nullptr), "gesher_binder_endpoint_owner_v1")));
  Binding* binding = factory ? factory() : &g_binding;
  binding->Initialize();
  return *binding;
}
}  // namespace gesher::binder_endpoint

#ifdef GESHER_FORK_LOCK
GESHER_FORK_LOCK(gesher::kForkOrderSubsystem, std::mutex, gesher::binder_endpoint::g_binding.ForkMutex());
#endif
