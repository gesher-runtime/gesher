// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace gesher::media {

struct VideoCrop {
  int left, top, right, bottom;
};

// MediaCodec modes 1/2 are scale-to-fit and scale-to-fit-with-cropping. Keep
// decoded pixels intact: the display consumer selects this source rectangle.
// Integer centering gives an odd discarded pixel to the right/bottom, as
// Android's GLConsumer does. Wide arithmetic keeps valid dimensions bounded.
inline VideoCrop VideoDisplayCrop(int width, int height, int target_width,
                                  int target_height, int mode) {
  if (width <= 0 || height <= 0 || target_width <= 0 || target_height <= 0)
    return {0, 0, 0, 0};
  if (mode != 2) return {0, 0, width, height};
  const int64_t across = static_cast<int64_t>(width) * target_height;
  const int64_t down = static_cast<int64_t>(height) * target_width;
  if (across > down) {
    const int kept = static_cast<int>(down / target_height);
    const int left = (width - kept) / 2;
    return {left, 0, left + kept, height};
  }
  if (across < down) {
    const int kept = static_cast<int>(across / target_width);
    const int top = (height - kept) / 2;
    return {0, top, width, top + kept};
  }
  return {0, 0, width, height};
}

}  // namespace gesher::media
