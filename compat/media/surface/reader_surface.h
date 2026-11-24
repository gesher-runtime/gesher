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
// A Java HardwareBuffer of an image's RGBA AHardwareBuffer (a gesher/rgba_buffers.h
// handle), for Image.getHardwareBuffer -> Bitmap.wrapHardwareBuffer. It holds a
// reference to the buffer; `width` and `height` are the buffer's.
jobject ReaderHardwareBuffer(JNIEnv* env, void* rgba, int width, int height);
// The AHardwareBuffer* of such a Java HardwareBuffer, for AHardwareBuffer_fromHardwareBuffer:
// null when `buffer` is not one or is closed. It lives as long as the Java object,
// which owns a reference, so no reference is taken for the caller.
void* ReaderHardwareBufferNative(JNIEnv* env, jobject buffer);
}  // namespace gesher::media
int register_gesher_reader_buffers(JNIEnv* env);
