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
// A fence that signals once the GPU has finished what ANGLE's device was given
// before the call (an app's draws that sample an EGLImage of a buffer's shared
// texture): the device's queue is flushed and a completion event registered under
// ANGLE's lock (as for CreateCurrentFence), without waiting. The fence owns the
// event, which stays alive until DXGI signals it. Callable from any thread with
// no EGL context, but it takes ANGLE's lock: never call it holding a lock that
// code running under ANGLE's lock can take. Null without ANGLE's dispatch, or on
// failure.
std::shared_ptr<NativeFence> CreateAngleFence();
}  // namespace gesher::bionic::gpu
