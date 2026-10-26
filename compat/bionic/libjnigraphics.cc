// Gesher: libjnigraphics.so's bitmap APIs for loaded Android libraries.
// hwui owns the real Bitmap storage; the ABitmap API pins that storage while
// native code holds its pixels. Only the entry/callback ABIs and the loaded
// library's JNIEnv proxy need conversion here (ADR 0062).

#include "abi_types.h"
#include "jni_bridge.h"

#include <android/bitmap.h>
#include <android/graphics/bitmap.h>
#include <gesher/bionic_shim.h>

namespace gesher::bionic {
namespace {

int BIONIC_SYSV GetInfo(JNIEnv* env, jobject object, AndroidBitmapInfo* info) {
  if (env == nullptr || object == nullptr) return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  if (info != nullptr) *info = ABitmap_getInfoFromJava(RealJniEnv(env), object);
  return ANDROID_BITMAP_RESULT_SUCCESS;
}

int32_t BIONIC_SYSV GetDataSpace(JNIEnv* env, jobject object) {
  if (env == nullptr || object == nullptr) return 0;  // ADATASPACE_UNKNOWN
  android::graphics::Bitmap bitmap(RealJniEnv(env), object);
  return bitmap.isValid() ? static_cast<int32_t>(bitmap.getDataSpace()) : 0;
}

int BIONIC_SYSV LockPixels(JNIEnv* env, jobject object, void** pixels) {
  if (env == nullptr || object == nullptr) return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  android::graphics::Bitmap bitmap(RealJniEnv(env), object);
  void* address = bitmap.isValid() ? bitmap.getPixels() : nullptr;
  if (address == nullptr) return ANDROID_BITMAP_RESULT_JNI_EXCEPTION;
  ABitmap_acquireRef(bitmap.get());
  if (pixels != nullptr) *pixels = address;
  return ANDROID_BITMAP_RESULT_SUCCESS;
}

int BIONIC_SYSV UnlockPixels(JNIEnv* env, jobject object) {
  if (env == nullptr || object == nullptr) return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  android::graphics::Bitmap bitmap(RealJniEnv(env), object);
  if (!bitmap.isValid()) return ANDROID_BITMAP_RESULT_JNI_EXCEPTION;
  bitmap.notifyPixelsChanged();
  ABitmap_releaseRef(bitmap.get());
  return ANDROID_BITMAP_RESULT_SUCCESS;
}

int BIONIC_SYSV GetHardwareBuffer(JNIEnv* env, jobject object, AHardwareBuffer** buffer) {
  if (env == nullptr || object == nullptr || buffer == nullptr) return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  android::graphics::Bitmap bitmap(RealJniEnv(env), object);
  if (!bitmap.isValid()) return ANDROID_BITMAP_RESULT_JNI_EXCEPTION;
  *buffer = bitmap.getHardwareBuffer();
  return *buffer != nullptr ? ANDROID_BITMAP_RESULT_SUCCESS : ANDROID_BITMAP_RESULT_BAD_PARAMETER;
}

using WriteCallback = bool(BIONIC_SYSV*)(void*, const void*, size_t);
struct Writer {
  void* context;
  WriteCallback callback;
};

bool Write(void* context, const void* data, size_t size) {
  auto* writer = static_cast<Writer*>(context);
  EnterLoadedCode();
  return writer->callback(writer->context, data, size);
}

int BIONIC_SYSV Compress(const AndroidBitmapInfo* info, int32_t data_space, const void* pixels,
                         int32_t format, int32_t quality, void* context, WriteCallback callback) {
  if (info == nullptr || pixels == nullptr || callback == nullptr || quality < 0 || quality > 100) {
    return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  }
  Writer writer{context, callback};
  return ABitmap_compress(info, static_cast<ADataSpace>(data_space), pixels,
                          static_cast<AndroidBitmapCompressFormat>(format), quality, &writer, Write);
}

[[maybe_unused]] const bool registered = [] {
#define E(name, function) AddBionicSymbol(name, reinterpret_cast<void*>(&function))
  E("AndroidBitmap_getInfo", GetInfo);
  E("AndroidBitmap_getDataSpace", GetDataSpace);
  E("AndroidBitmap_lockPixels", LockPixels);
  E("AndroidBitmap_unlockPixels", UnlockPixels);
  E("AndroidBitmap_getHardwareBuffer", GetHardwareBuffer);
  E("AndroidBitmap_compress", Compress);
#undef E
  return true;
}();

}  // namespace
}  // namespace gesher::bionic
