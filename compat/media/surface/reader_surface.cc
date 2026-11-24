#include "reader_surface.h"
#include "queue_surface.h"
#include <android_runtime/android_view_Surface.h>
#include <gesher/rgba_buffers.h>
#include <GraphicsJNI.h>
#include <hwui/Bitmap.h>
#include <nativehelper/JNIHelp.h>
#include <atomic>
#include <cstring>
#include <memory>

namespace gesher::bionic {
int RegisterSyncFenceNatives(JNIEnv* env);  // hardware_buffer_java.cc
}

namespace gesher::media {
namespace {
// A Java HardwareBuffer of a reader image holds a reference to the image's RGBA
// AHardwareBuffer (<gesher/rgba_buffers.h>), which the reader's pool will not
// write into while it is held. Its immutable Bitmap conversion works on both
// Gesher's CPU and SkiaGL display pipelines.
struct Buffer {
  void* ahb;  // a reference
  int width, height;
  Buffer(void* b, int w, int h) : ahb(b), width(w), height(h) { gesher::bionic::ahb::RetainRgbaBuffer(b); }
  ~Buffer() { gesher::bionic::ahb::ReleaseRgbaBuffer(ahb); }
};
jclass buffer_class;
jmethodID buffer_constructor;
jfieldID buffer_pointer;
Buffer* Of(jlong ptr) { return reinterpret_cast<Buffer*>(ptr); }
void DeleteBuffer(void* ptr) { delete static_cast<Buffer*>(ptr); }
jlong JNICALL Finalizer(JNIEnv*, jclass) { return reinterpret_cast<jlong>(&DeleteBuffer); }
jint JNICALL Width(JNIEnv*, jclass, jlong ptr) { return Of(ptr)->width; }
jint JNICALL Height(JNIEnv*, jclass, jlong ptr) { return Of(ptr)->height; }
jint JNICALL Format(JNIEnv*, jclass, jlong) { return 1; }
jint JNICALL Layers(JNIEnv*, jclass, jlong) { return 1; }
jlong JNICALL Usage(JNIEnv*, jclass, jlong ptr) {
  return static_cast<jlong>(gesher::bionic::ahb::RgbaBufferUsage(Of(ptr)->ahb));
}
// @CriticalNative omits JNIEnv and jclass; the accessors above are @FastNative.
jlong JNICALL Id(jlong ptr) { return static_cast<jlong>(gesher::bionic::ahb::RgbaBufferId(Of(ptr)->ahb)); }
jlong JNICALL Estimate(jlong ptr) { return static_cast<jlong>(Of(ptr)->width) * Of(ptr)->height * 4; }
jboolean JNICALL Supported(JNIEnv*, jclass, jint, jint, jint, jint, jlong) { return JNI_FALSE; }
jlong JNICALL Create(JNIEnv* env, jclass, jint, jint, jint, jint, jlong) {
  jniThrowException(env, "java/lang/UnsupportedOperationException", "HardwareBuffer allocation is not supported; only reader snapshots are available");
  return 0;
}
jlong JNICALL FromGraphicBuffer(JNIEnv* env, jclass, jobject) {
  jniThrowException(env, "java/lang/UnsupportedOperationException", "GraphicBuffer import is not supported on Gesher");
  return 0;
}
void JNICALL WriteParcel(JNIEnv* env, jclass, jlong, jobject) {
  jniThrowException(env, "java/lang/UnsupportedOperationException", "HardwareBuffer parcel transport is not supported on Gesher");
}
jlong JNICALL ReadParcel(JNIEnv* env, jclass, jobject) {
  jniThrowException(env, "java/lang/UnsupportedOperationException", "HardwareBuffer parcel transport is not supported on Gesher");
  return 0;
}
jobject JNICALL Wrap(JNIEnv* env, jclass, jobject buffer, jlong color_space) {
  jlong ptr = env->GetLongField(buffer, buffer_pointer);
  if (ptr == 0) {
    jniThrowException(env, "java/lang/IllegalStateException", "HardwareBuffer is closed");
    return nullptr;
  }
  Buffer* data = Of(ptr);
  SkBitmap pixels;
  auto info = SkImageInfo::Make(data->width, data->height, kRGBA_8888_SkColorType,
      kPremul_SkAlphaType, GraphicsJNI::getNativeColorSpace(color_space));
  if (!pixels.tryAllocPixels(info)) {
    jniThrowException(env, "java/lang/OutOfMemoryError", "Cannot allocate reader bitmap");
    return nullptr;
  }
  for (int y = 0; y < data->height; y++)
    memcpy(pixels.getAddr(0, y), gesher::bionic::ahb::RgbaBufferPixels(data->ahb) + static_cast<size_t>(y) * data->width * 4,
           static_cast<size_t>(data->width) * 4);
  pixels.setImmutable();
  auto bitmap = android::Bitmap::createFrom(info, *pixels.pixelRef());
  return android::bitmap::createBitmap(env, bitmap.release(), android::bitmap::kBitmapCreateFlag_Premultiplied);
}
#define N(fn) reinterpret_cast<void*>(&(fn))
const JNINativeMethod methods[] = {
  {"nGetNativeFinalizer", "()J", N(Finalizer)},
  {"nGetWidth", "(J)I", N(Width)}, {"nGetHeight", "(J)I", N(Height)},
  {"nGetFormat", "(J)I", N(Format)}, {"nGetLayers", "(J)I", N(Layers)},
  {"nGetUsage", "(J)J", N(Usage)}, {"nGetId", "(J)J", N(Id)},
  {"nEstimateSize", "(J)J", N(Estimate)}, {"nIsSupported", "(IIIIJ)Z", N(Supported)},
  {"nCreateHardwareBuffer", "(IIIIJ)J", N(Create)},
  {"nCreateFromGraphicBuffer", "(Landroid/graphics/GraphicBuffer;)J", N(FromGraphicBuffer)},
  {"nWriteHardwareBufferToParcel", "(JLandroid/os/Parcel;)V", N(WriteParcel)},
  {"nReadHardwareBufferFromParcel", "(Landroid/os/Parcel;)J", N(ReadParcel)},
};
const JNINativeMethod wrap_method[] = {
  {"nativeWrapHardwareBufferBitmap", "(Landroid/hardware/HardwareBuffer;J)Landroid/graphics/Bitmap;", N(Wrap)},
};
#undef N
class NativeReaderSurface final : public ReaderSurface {
 public:
  NativeReaderSurface(int width, int height, ReaderFrame listener)
      : surface_(android::sp<QueueSurface>::make(width, height)) {
    surface_->SetListener([surface = surface_.get(), listener = std::move(listener)] {
      auto frame = surface->Latest();
      if (frame != nullptr)
        listener(frame->pixels.data(), frame->width, frame->height, frame->timestamp_ns);
    });
  }
  ~NativeReaderSurface() override { Close(); }
  jobject JavaSurface(JNIEnv* env) override {
    return android::android_view_Surface_createFromSurface(env, surface_);
  }
  void Close() override { surface_->Abandon(); }
 private:
  android::sp<QueueSurface> surface_;
};
}  // namespace
std::shared_ptr<ReaderSurface> MakeReaderSurface(int width, int height, ReaderFrame listener) {
  return std::make_shared<NativeReaderSurface>(width, height, std::move(listener));
}
void* ReaderHardwareBufferNative(JNIEnv* env, jobject buffer) {
  if (buffer == nullptr || buffer_class == nullptr || !env->IsInstanceOf(buffer, buffer_class))
    return nullptr;
  jlong ptr = env->GetLongField(buffer, buffer_pointer);
  return ptr != 0 ? Of(ptr)->ahb : nullptr;
}
jobject ReaderHardwareBuffer(JNIEnv* env, void* rgba, int width, int height) {
  auto buffer = std::make_unique<Buffer>(rgba, width, height);
  jobject result = env->NewObject(buffer_class, buffer_constructor, reinterpret_cast<jlong>(buffer.get()));
  if (result != nullptr && !env->ExceptionCheck()) buffer.release();
  return result;
}
}  // namespace gesher::media

int register_gesher_reader_buffers(JNIEnv* env) {
  using namespace gesher::media;
  jclass cls = env->FindClass("android/hardware/HardwareBuffer");
  if (cls == nullptr) { env->ExceptionClear(); return 0; }
  buffer_class = static_cast<jclass>(env->NewGlobalRef(cls));
  buffer_constructor = env->GetMethodID(cls, "<init>", "(J)V");
  buffer_pointer = env->GetFieldID(cls, "mNativeObject", "J");
  if (env->RegisterNatives(cls, methods, sizeof(methods) / sizeof(methods[0])) != 0) return -1;
  env->DeleteLocalRef(cls);
  if (gesher::bionic::RegisterSyncFenceNatives(env) != 0) return -1;
  cls = env->FindClass("android/graphics/Bitmap");
  if (cls == nullptr) return -1;
  int result = env->RegisterNatives(cls, wrap_method, 1);
  env->DeleteLocalRef(cls);
  return result;
}
