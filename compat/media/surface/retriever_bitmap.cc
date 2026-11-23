#include "retriever_bitmap.h"

#include <GraphicsJNI.h>
#include <hwui/Bitmap.h>
#include <nativehelper/JNIHelp.h>

#include "../video/retriever_pixels.h"

namespace gesher::media {
jobject AllocateRetrieverBitmap(JNIEnv* env, const VideoFrame& frame, jobject config,
                                int width, int height, int rotation, bool scaled) {
  if (width <= 0 || height <= 0) {
    jniThrowException(env, "java/lang/IllegalArgumentException", "width and height must be > 0");
    return nullptr;
  }
  const SkColorType color_type = GraphicsJNI::getNativeBitmapColorType(env, config);
  if (env->ExceptionCheck()) return nullptr;
  SkBitmap pixels;
  if (!pixels.setInfo(SkImageInfo::Make(width, height, color_type, kPremul_SkAlphaType,
                                       SkColorSpace::MakeSRGB()))) return nullptr;
  auto bitmap = android::Bitmap::allocateHeapBitmap(&pixels);
  if (bitmap == nullptr) {
    // The previous initial bitmap allocation returned null on OOM; the later
    // createScaledBitmap allocation propagated OutOfMemoryError to the caller.
    if (scaled) jniThrowException(env, "java/lang/OutOfMemoryError", "Cannot allocate retriever bitmap");
    return nullptr;
  }
  if (!RenderRetrieverBitmap(frame, rotation, &pixels)) return nullptr;
  int flags = android::bitmap::kBitmapCreateFlag_Premultiplied;
  if (scaled) flags |= android::bitmap::kBitmapCreateFlag_Mutable;
  jobject result = android::bitmap::createBitmap(env, bitmap.release(), flags);
  if (result == nullptr && !scaled && env->ExceptionCheck()) env->ExceptionClear();
  return result;
}
}  // namespace gesher::media
