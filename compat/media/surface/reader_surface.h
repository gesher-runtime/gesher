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

struct ReaderSnapshot {
  int width, height;
  const uint8_t* pixels;  // RGBA8888, rows of width * 4 bytes
};
// The native buffer (an NDK AHardwareBuffer*, for AHardwareBuffer_fromHardwareBuffer)
// of a Java HardwareBuffer that ReaderHardwareBuffer made, or null when `buffer` is
// not one or is closed. The first call makes it with `make` from the snapshot's
// pixels (null: it cannot be made, nothing is cached); later calls return the same
// one. The snapshot owns it: `release` runs when the snapshot is freed, so it lives
// as long as the Java object and fromHardwareBuffer takes no reference.
void* ReaderSnapshotNativeBuffer(JNIEnv* env, jobject buffer, void* (*make)(const ReaderSnapshot&),
                                 void (*release)(void*));
}  // namespace gesher::media
int register_gesher_reader_buffers(JNIEnv* env);
