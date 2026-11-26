// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

struct ANativeWindow;

namespace gesher::media {
// CameraService sends real NV12 camera output. An empty result means the
// window is not an NDK YUV consumer; otherwise return the delivery status.
std::optional<int> QueueNdkReaderCameraFrame(ANativeWindow* window, const uint8_t* nv12,
                                             size_t size, int width, int height,
                                             int64_t timestamp_ns);
}  // namespace gesher::media
