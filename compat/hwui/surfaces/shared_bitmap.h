// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <hwui/Bitmap.h>

namespace gesher::hwui {

// One owned descriptor and mapping. Allocate keeps its initialization mapping
// writable, as Android's AshmemPixelAllocator does, but seals the region against
// resizing, descriptor writes and any future writable shared mapping. Adopted
// Bitmap storage owns both resources until its final SkPixelRef is released.
class SharedBitmapRegion final {
 public:
  SharedBitmapRegion() = default;
  ~SharedBitmapRegion();
  SharedBitmapRegion(const SharedBitmapRegion&) = delete;
  SharedBitmapRegion& operator=(const SharedBitmapRegion&) = delete;
  SharedBitmapRegion(SharedBitmapRegion&& other) noexcept;
  SharedBitmapRegion& operator=(SharedBitmapRegion&&) = delete;

  static SharedBitmapRegion Allocate(size_t size);
  // The caller retains fd. The returned region owns an independent duplicate.
  // Check the actual file capacity before mapping the declared Parcel size.
  static SharedBitmapRegion Map(int fd, size_t size, bool read_only);

  explicit operator bool() const { return address_ != nullptr; }
  void* pixels() const { return address_; }
  int fd() const { return fd_; }
  size_t size() const { return size_; }
  sk_sp<android::Bitmap> TakeBitmap(const SkImageInfo& info, size_t row_bytes,
                                    bool read_only);

 private:
  int fd_ = -1;
  void* address_ = nullptr;
  size_t size_ = 0;
};

}  // namespace gesher::hwui
