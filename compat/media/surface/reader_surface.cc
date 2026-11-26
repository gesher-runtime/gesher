#include "reader_surface.h"
#include "queue_surface.h"
#include "../../bionic/hardware_buffer_base.h"
#include <android_runtime/android_view_Surface.h>
#include <gesher/rgba_buffers.h>
#include <GraphicsJNI.h>
#include <hwui/Bitmap.h>
#include <nativehelper/JNIHelp.h>
#include <cstring>
#include <memory>
#include <new>

namespace gesher::bionic {
int RegisterSyncFenceNatives(JNIEnv* env);  // hardware_buffer_java.cc
}

namespace gesher::media {
namespace {
namespace ahb = gesher::bionic::ahb;

// The Java object owns one reference to the same storage used by the NDK.
struct JavaBuffer {
  ahb::Buffer* buffer;
  bool from_reader;
  ~JavaBuffer() {
    if (from_reader) ahb::NoteRgbaAppReleased(ahb::ToApp(buffer));
    ahb::Release(buffer);
  }
};
jclass buffer_class;
jmethodID buffer_constructor;
jfieldID buffer_pointer;
JavaBuffer* Of(jlong ptr) { return reinterpret_cast<JavaBuffer*>(ptr); }
void DeleteBuffer(void* ptr) { delete static_cast<JavaBuffer*>(ptr); }
jlong JNICALL Finalizer(JNIEnv*, jclass) { return reinterpret_cast<jlong>(&DeleteBuffer); }
jint JNICALL Width(JNIEnv*, jclass, jlong ptr) { return ahb::Desc(Of(ptr)->buffer).width; }
jint JNICALL Height(JNIEnv*, jclass, jlong ptr) { return ahb::Desc(Of(ptr)->buffer).height; }
jint JNICALL Format(JNIEnv*, jclass, jlong ptr) { return ahb::Desc(Of(ptr)->buffer).format; }
jint JNICALL Layers(JNIEnv*, jclass, jlong ptr) { return ahb::Desc(Of(ptr)->buffer).layers; }
jlong JNICALL Usage(JNIEnv*, jclass, jlong ptr) {
  return static_cast<jlong>(ahb::Desc(Of(ptr)->buffer).usage);
}
// @CriticalNative omits JNIEnv and jclass; the accessors above are @FastNative.
jlong JNICALL Id(jlong ptr) { return static_cast<jlong>(ahb::BufferId(Of(ptr)->buffer)); }
jlong JNICALL Estimate(jlong ptr) { return static_cast<jlong>(ahb::AllocationSize(Of(ptr)->buffer)); }

AHardwareBuffer_Desc Description(jint width, jint height, jint format, jint layers, jlong usage) {
  return {static_cast<uint32_t>(width), static_cast<uint32_t>(height),
          static_cast<uint32_t>(layers), static_cast<uint32_t>(format),
          static_cast<uint64_t>(usage), 0, 0, 0};
}

jboolean JNICALL Supported(JNIEnv*, jclass, jint width, jint height, jint format, jint layers,
                           jlong usage) {
  return ahb::IsBufferSupported(Description(width, height, format, layers, usage));
}

jlong JNICALL Create(JNIEnv*, jclass, jint width, jint height, jint format, jint layers,
                     jlong usage) {
  ahb::Buffer* buffer = ahb::AllocateBuffer(Description(width, height, format, layers, usage));
  if (buffer == nullptr) return 0;  // HardwareBuffer.create throws IllegalArgumentException.
  auto* java_buffer = new (std::nothrow) JavaBuffer{buffer, false};
  if (java_buffer == nullptr) ahb::Release(buffer);
  return reinterpret_cast<jlong>(java_buffer);
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
  JavaBuffer* data = Of(ptr);
  // This snapshot path only handles ImageReader's RGBA storage.
  if (!data->from_reader) return nullptr;
  const auto& desc = ahb::Desc(data->buffer);
  SkBitmap pixels;
  auto info = SkImageInfo::Make(desc.width, desc.height, kRGBA_8888_SkColorType,
      kPremul_SkAlphaType, GraphicsJNI::getNativeColorSpace(color_space));
  if (!pixels.tryAllocPixels(info)) {
    jniThrowException(env, "java/lang/OutOfMemoryError", "Cannot allocate reader bitmap");
    return nullptr;
  }
  const uint8_t* source = ahb::RgbaBufferPixels(ahb::ToApp(data->buffer));
  for (uint32_t y = 0; y < desc.height; y++)
    memcpy(pixels.getAddr(0, y), source + static_cast<size_t>(y) * desc.stride * 4,
           static_cast<size_t>(desc.width) * 4);
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
void* HardwareBufferNative(JNIEnv* env, jobject buffer) {
  if (buffer == nullptr || buffer_class == nullptr || !env->IsInstanceOf(buffer, buffer_class))
    return nullptr;
  jlong ptr = env->GetLongField(buffer, buffer_pointer);
  return ptr != 0 ? ahb::ToApp(Of(ptr)->buffer) : nullptr;
}
jobject ReaderHardwareBuffer(JNIEnv* env, void* rgba) {
  ahb::Buffer* backing = ahb::FromApp(rgba);
  if (backing == nullptr) return nullptr;
  auto buffer = std::unique_ptr<JavaBuffer>(new (std::nothrow) JavaBuffer{backing, true});
  if (buffer == nullptr) {
    jniThrowException(env, "java/lang/OutOfMemoryError", "Cannot allocate reader HardwareBuffer");
    return nullptr;
  }
  ahb::Acquire(backing);
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
