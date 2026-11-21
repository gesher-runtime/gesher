// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

// Private Microsoft x64 -> System V call record shared with thunks.S.
// Keep the scalar result offsets stable; binary128 also uses XMM0's high word.
struct HostCall {
  void* fn;
  const uint64_t* gpr;    // X0..X5 -> rdi,rsi,rdx,rcx,r8,r9
  const uint64_t* xmm;    // V0..V7 low 64 bits -> xmm0..xmm7
  const uint64_t* stack;  // X6, X7, then guest stack arguments
  uint64_t nstack;
  uint64_t out_xmm0;
  uint64_t out_rdx;
  uint64_t out_xmm0_hi;
};

static_assert(offsetof(HostCall, fn) == 0);
static_assert(offsetof(HostCall, gpr) == 8);
static_assert(offsetof(HostCall, xmm) == 16);
static_assert(offsetof(HostCall, stack) == 24);
static_assert(offsetof(HostCall, nstack) == 32);
static_assert(offsetof(HostCall, out_xmm0) == 40);
static_assert(offsetof(HostCall, out_rdx) == 48);
static_assert(offsetof(HostCall, out_xmm0_hi) == 56);
static_assert(sizeof(HostCall) == 64);
