// Gesher (surfaces track, ADR 0056; docs/graphics.md "Surfaces"): the
// natives this track adds over hwui's, registered at the end of
// register_gesher_graphics (compat/hwui/graphics_runtime.cc), so they replace
// hwui's Linux-only ones:
//  - Bitmap.nativeWriteToParcel / nativeCreateFromParcel (bitmap_parcel.cc);
//  - YuvImage.nativeCompressToJpeg(R) (yuv_image.cc).
// Each is skipped, with a log line, when its class is not on the class path.

#include <jni.h>

#include <log/log.h>

int register_gesher_bitmap_parcel(JNIEnv* env);
int register_gesher_yuv_image(JNIEnv* env);

namespace {

bool HasClass(JNIEnv* env, const char* name) {
  jclass c = env->FindClass(name);
  if (c == nullptr) {
    env->ExceptionClear();
    return false;
  }
  env->DeleteLocalRef(c);
  return true;
}

}  // namespace

int register_gesher_surfaces(JNIEnv* env) {
  const struct {
    const char* class_name;
    int (*fn)(JNIEnv*);
  } kRegistrations[] = {
      {"android/graphics/Bitmap", register_gesher_bitmap_parcel},
      {"android/graphics/YuvImage", register_gesher_yuv_image},
  };
  for (const auto& r : kRegistrations) {
    if (!HasClass(env, r.class_name)) {
      ALOGI("surfaces: %s is not on the class path; its natives are not registered", r.class_name);
      continue;
    }
    if (r.fn(env) < 0) {
      ALOGE("surfaces: registering the natives of %s failed", r.class_name);
      return -1;
    }
  }
  return 0;
}
