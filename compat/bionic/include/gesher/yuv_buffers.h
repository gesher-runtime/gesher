// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <android/hardware_buffer.h>

namespace gesher::bionic::ahb {

// CPU-readable I420 storage. The producer may write the plane addresses before
// publishing the buffer. The caller owns one reference.
AHardwareBuffer* AllocateYuv420Buffer(int32_t width, int32_t height, uint64_t usage,
                                      AHardwareBuffer_Planes* planes);
// Drop an image's reference to an RGBA or YUV hardware buffer.
void ReleaseImageBuffer(AHardwareBuffer* buffer);

}  // namespace gesher::bionic::ahb
