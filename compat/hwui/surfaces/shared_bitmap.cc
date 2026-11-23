// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#include "shared_bitmap.h"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace gesher::hwui {

SharedBitmapRegion::~SharedBitmapRegion() {
  const int saved = errno;
  if (address_ != nullptr) munmap(address_, size_);
  if (fd_ >= 0) close(fd_);
  errno = saved;
}

SharedBitmapRegion::SharedBitmapRegion(SharedBitmapRegion&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      address_(std::exchange(other.address_, nullptr)),
      size_(std::exchange(other.size_, 0)) {}

SharedBitmapRegion SharedBitmapRegion::Allocate(size_t size) {
  SharedBitmapRegion region;
  if (size == 0 || size > INT_MAX) {
    errno = EINVAL;
    return {};
  }
  region.fd_ = memfd_create("bitmap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (region.fd_ < 0) return {};
#ifdef GESHER
  const int resized = gesher_ftruncate(region.fd_, static_cast<off_t>(size));
#else
  const int resized = ftruncate(region.fd_, static_cast<off_t>(size));
#endif
  if (resized != 0) return {};
  void* address = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, region.fd_, 0);
  if (address == MAP_FAILED) return {};
  region.address_ = address;
  region.size_ = size;
  if (fcntl(region.fd_, F_ADD_SEALS,
            F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_FUTURE_WRITE) != 0) return {};
  return region;
}

SharedBitmapRegion SharedBitmapRegion::Map(int fd, size_t size, bool read_only) {
  SharedBitmapRegion region;
  if (fd < 0 || size == 0 || size > INT_MAX) {
    errno = EINVAL;
    return {};
  }
  region.fd_ = fcntl(fd, F_DUPFD_CLOEXEC, 0);
  if (region.fd_ < 0) return {};
  struct stat status {};
  if (fstat(region.fd_, &status) != 0) return {};
  if (status.st_size < 0 || static_cast<uint64_t>(status.st_size) < size) {
    errno = EOVERFLOW;
    return {};
  }
  const int prot = PROT_READ | (read_only ? 0 : PROT_WRITE);
  void* address = mmap(nullptr, size, prot, MAP_SHARED, region.fd_, 0);
  if (address == MAP_FAILED) return {};
  region.address_ = address;
  region.size_ = size;
  return region;
}

sk_sp<android::Bitmap> SharedBitmapRegion::TakeBitmap(const SkImageInfo& info,
                                                     size_t row_bytes, bool read_only) {
  if (!*this) return nullptr;
  auto bitmap = android::Bitmap::createFrom(info, row_bytes, fd_, address_, size_, read_only);
  if (bitmap != nullptr) {
    fd_ = -1;
    address_ = nullptr;
    size_ = 0;
  }
  return bitmap;
}

}  // namespace gesher::hwui

// The narrow GESHER branch of the pinned hwui patch delegates its private
// allocation overload here. The original public allocator, Skia copy, immutable
// flag and Bitmap/SkPixelRef ownership are unchanged.
sk_sp<android::Bitmap> android::Bitmap::allocateAshmemBitmap(
    size_t size, const SkImageInfo& info, size_t row_bytes) {
  auto region = gesher::hwui::SharedBitmapRegion::Allocate(size);
  return region.TakeBitmap(info, row_bytes, false);
}
