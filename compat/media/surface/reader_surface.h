// ImageReader's native producer, shared by Canvas/NDK/EGL writers.
#pragma once
#include <jni.h>
#include <cstdint>
#include <functional>
#include <memory>

namespace gesher::media {
using ReaderFrame = std::function<void(const uint32_t*, int, int, int64_t)>;
class ReaderSurface {
 public:
  virtual ~ReaderSurface() = default;
  virtual jobject JavaSurface(JNIEnv* env) = 0;
  virtual void Close() = 0;
};
std::shared_ptr<ReaderSurface> MakeReaderSurface(int width, int height, ReaderFrame listener);
// A retained CPU snapshot for Image.getHardwareBuffer -> Bitmap.wrapHardwareBuffer.
jobject ReaderHardwareBuffer(JNIEnv* env, const uint8_t* pixels, int width, int height);
}  // namespace gesher::media
int register_gesher_reader_buffers(JNIEnv* env);
