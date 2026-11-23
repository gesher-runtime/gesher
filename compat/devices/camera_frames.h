// Native producer entry for the ImageReader queue in camera_frames.cc.
#pragma once

#include <jni.h>
#include <cstdint>

namespace gesher::media {
struct VideoFrame;
}

namespace android {
// Copies the visible I420 frame into reader-owned storage. The caller retains
// its frame only until this synchronous call returns; writable Image planes
// never alias decoder output. Returns an Android status, or 1 for an unsupported
// reader format, matching CameraFrames.nativeDeliver.
int QueueImageReaderVideoFrame(JNIEnv* env, jobject surface,
                               const gesher::media::VideoFrame& frame,
                               int64_t timestamp_ns);
}  // namespace android
