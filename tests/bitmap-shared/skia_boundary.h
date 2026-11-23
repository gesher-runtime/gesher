// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Declared host Skia/refcount boundary. The pinned hwui Bitmap class declaration,
// allocation/adoption/destruction methods and production Parcel implementation
// are compiled separately. This does not emulate Skia rendering or compression.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

class SkRefCnt {
 public:
  void ref() const { ++refs_; }
  void unref() const { if (--refs_ == 0) delete this; }
 protected:
  virtual ~SkRefCnt() = default;
 private:
  mutable std::atomic<int> refs_{1};
};
template<class T> class sk_sp {
 public:
  sk_sp() = default;
  sk_sp(std::nullptr_t) {}
  explicit sk_sp(T* value) : value_(value) {}
  sk_sp(const sk_sp& other) : value_(other.value_) { if (value_) value_->ref(); }
  sk_sp(sk_sp&& other) noexcept : value_(other.release()) {}
  ~sk_sp() { if (value_) value_->unref(); }
  sk_sp& operator=(sk_sp value) { std::swap(value_, value.value_); return *this; }
  T* get() const { return value_; }
  T* operator->() const { return value_; }
  T& operator*() const { return *value_; }
  explicit operator bool() const { return value_ != nullptr; }
  bool operator==(std::nullptr_t) const { return !value_; }
  bool operator!=(std::nullptr_t) const { return value_ != nullptr; }
  void reset(T* value = nullptr) { *this = sk_sp(value); }
  T* release() { return std::exchange(value_, nullptr); }
 private:
  T* value_ = nullptr;
};
template<class T> sk_sp<T> sk_ref_sp(T* value) { if (value) value->ref(); return sk_sp<T>(value); }

class SkData : public SkRefCnt {
 public:
  explicit SkData(std::vector<uint8_t> value) : bytes_(std::move(value)) {}
  const void* data() const { return bytes_.data(); }
  size_t size() const { return bytes_.size(); }
 private:
  std::vector<uint8_t> bytes_;
};
class SkColorSpace : public SkRefCnt {
 public:
  sk_sp<SkData> serialize() const { return sk_sp<SkData>(new SkData({1, 3, 5, 7})); }
  static sk_sp<SkColorSpace> Deserialize(const void* bytes, size_t size) {
    const uint8_t expected[] = {1, 3, 5, 7};
    return size == sizeof(expected) && std::memcmp(bytes, expected, size) == 0
        ? sk_sp<SkColorSpace>(new SkColorSpace) : nullptr;
  }
};
enum SkColorType { kUnknown_SkColorType, kAlpha_8_SkColorType, kRGB_565_SkColorType,
  kARGB_4444_SkColorType, kN32_SkColorType, kRGBA_F16_SkColorType };
enum SkAlphaType { kUnknown_SkAlphaType, kOpaque_SkAlphaType, kPremul_SkAlphaType,
                   kUnpremul_SkAlphaType };
inline bool SkColorTypeValidateAlphaType(SkColorType color, SkAlphaType alpha, SkAlphaType* out) {
  if (color <= kUnknown_SkColorType || color > kRGBA_F16_SkColorType ||
      alpha <= kUnknown_SkAlphaType || alpha > kUnpremul_SkAlphaType) return false;
  *out = color == kRGB_565_SkColorType ? kOpaque_SkAlphaType : alpha;
  return true;
}
class SkImageInfo {
 public:
  static SkImageInfo Make(int width, int height, SkColorType color, SkAlphaType alpha,
                          sk_sp<SkColorSpace> space = nullptr) {
    SkImageInfo result; result.width_ = width; result.height_ = height;
    result.color_ = color; result.alpha_ = alpha; result.space_ = std::move(space); return result;
  }
  int width() const { return width_; }
  int height() const { return height_; }
  SkColorType colorType() const { return color_; }
  SkAlphaType alphaType() const { return alpha_; }
  SkColorSpace* colorSpace() const { return space_.get(); }
  sk_sp<SkColorSpace> refColorSpace() const { return space_; }
  int bytesPerPixel() const { return color_ == kAlpha_8_SkColorType ? 1 :
      color_ == kN32_SkColorType ? 4 : color_ == kRGBA_F16_SkColorType ? 8 : 2; }
  int shiftPerPixel() const { return bytesPerPixel() == 1 ? 0 : bytesPerPixel() == 2 ? 1 : bytesPerPixel() == 4 ? 2 : 3; }
  size_t minRowBytes() const { return size_t(width_) * bytesPerPixel(); }
  bool isOpaque() const { return alpha_ == kOpaque_SkAlphaType; }
  SkImageInfo makeAlphaType(SkAlphaType alpha) const { auto result = *this; result.alpha_ = alpha; return result; }
  SkImageInfo makeColorSpace(sk_sp<SkColorSpace> space) const { auto result = *this; result.space_ = std::move(space); return result; }
 private:
  int width_ = 0, height_ = 0;
  SkColorType color_ = kUnknown_SkColorType;
  SkAlphaType alpha_ = kUnknown_SkAlphaType;
  sk_sp<SkColorSpace> space_;
};
class SkPixelRef : public SkRefCnt {
 public:
  SkPixelRef(int width, int height, void* pixels, size_t row) : width_(width), height_(height), pixels_(pixels), row_(row) {}
  int width() const { return width_; }
  int height() const { return height_; }
  size_t rowBytes() const { return row_; }
  void* pixels() const { return pixels_; }
  void setImmutable() { immutable_ = true; }
  bool isImmutable() const { return immutable_; }
  uint32_t getGenerationID() const { return 1; }
  void android_only_reset(int width, int height, size_t row) { width_ = width; height_ = height; row_ = row; }
 private:
  int width_, height_;
  void* pixels_;
  size_t row_;
  bool immutable_ = false;
};
class SkBitmap {
 public:
  bool setInfo(const SkImageInfo& info, size_t row = 0) {
    SkAlphaType alpha;
    if (info.width() <= 0 || info.height() <= 0 ||
        !SkColorTypeValidateAlphaType(info.colorType(), info.alphaType(), &alpha)) return false;
    if (row == 0) row = info.minRowBytes();
    if (row < info.minRowBytes() || row % info.bytesPerPixel() != 0) return false;
    info_ = info; row_ = row; return true;
  }
  const SkImageInfo& info() const { return info_; }
  int width() const { return info_.width(); }
  int height() const { return info_.height(); }
  size_t rowBytes() const { return row_; }
  SkColorType colorType() const { return info_.colorType(); }
  SkAlphaType alphaType() const { return info_.alphaType(); }
  SkColorSpace* colorSpace() const { return info_.colorSpace(); }
  bool isImmutable() const { return pixels_ && pixels_->isImmutable(); }
  void* getPixels() const { return pixels_ ? pixels_->pixels() : nullptr; }
  size_t computeByteSize() const { return height() > 0 ? size_t(height() - 1) * row_ + info_.minRowBytes() : 0; }
  void setPixelRef(sk_sp<SkPixelRef> pixels, int, int) { pixels_ = std::move(pixels); }
  template<class T> void setPixelRef(sk_sp<T> pixels, int x, int y) {
    setPixelRef(sk_sp<SkPixelRef>(pixels.release()), x, y);
  }
 private:
  SkImageInfo info_;
  size_t row_ = 0;
  sk_sp<SkPixelRef> pixels_;
};
class SkImage : public SkRefCnt {};
class SkColorFilter;
class SkRect;
namespace android {
// Gainmaps are unused in this CPU pixel/Parcel fixture; the actual framework
// gainmap Java path is unchanged and is not qualified by this boundary.
template<class T> class sp {
 public:
  T* get() const { return nullptr; }
};
}
