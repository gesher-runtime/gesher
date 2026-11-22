// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gesher/native_fence.h>

namespace gesher::bionic::gpu {
// The caller validates that its current EGL context belongs to the display.
// Submission uses ANGLE's mutex; completion and descriptor work is outside it.
// Local server waits rely on that same queue's existing command order.
std::shared_ptr<NativeFence> CreateCurrentFence(void* display, int* egl_error);
bool WaitCurrentFence(const std::shared_ptr<NativeFence>& fence, int* egl_error);
}  // namespace gesher::bionic::gpu
