#pragma once

#include <jni.h>

namespace gesher::media {
struct VideoFrame;

// Compiled with hwui so the retriever can populate the final Android Bitmap
// allocation directly, without Java arrays or an intermediate full-size image.
jobject AllocateRetrieverBitmap(JNIEnv* env, const VideoFrame& frame, jobject config,
                                int width, int height, int rotation, bool scaled);
}  // namespace gesher::media
