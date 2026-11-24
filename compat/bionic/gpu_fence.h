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
// Returns once the GPU has finished what ANGLE's device was given before the call
// (an app's draws that sample an EGLImage of a buffer's shared texture): the
// device's queue is flushed and a completion event registered under ANGLE's lock
// (as for CreateCurrentFence), and the wait is outside it. Callable from any
// thread, with no EGL context. False (not waited) without ANGLE's dispatch or on
// failure or timeout, after which the caller goes on unordered.
bool WaitAngleDeviceIdle(unsigned timeout_ms);
}  // namespace gesher::bionic::gpu
