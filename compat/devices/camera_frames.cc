// Gesher (devices track): where camera frames go (docs/devices.md).
//
// - android.media.ImageReader and its SurfaceImage: Gesher's own natives (the
//   framework's need libgui's BufferItemConsumer). A reader is a queue of
//   images in process memory; its Surface is a plain Java Surface that only
//   this file knows belongs to it (in-process consumers only: the camera, and
//   anything else that writes through CameraFrames).
// - android.hardware.camera2.utils.SurfaceUtils: a reader's size and format,
//   or a window surface's through its ANativeWindow.
// - com.gesher.server.CameraFrames (gesher-app.jar): the camera service hands
//   each frame (NV12) to a target Surface: converted for an ImageReader
//   (YUV_420_888 as I420, RGBA_8888, JPEG bytes the service encoded), or drawn
//   into a surface that has a native window (ANativeWindow_lock).

#include <dlfcn.h>
#include <gesher/builtin_library.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/scoped_local_ref.h>

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <vector>

#include "core_jni_helpers.h"
#include "camera_trace.h"

// libandroid_runtime's host ANativeWindow_fromSurface and the ANativeWindow
// functions dalvikvm defines (docs/graphics.md, docs/native-libs.md).
struct ANativeWindow;
struct ARect {
  int32_t left, top, right, bottom;
};
struct ANativeWindow_Buffer {
  int32_t width, height, stride, format;
  void* bits;
  uint32_t reserved[6];
};
extern "C" {
ANativeWindow* ANativeWindow_fromSurface(JNIEnv* env, jobject surface);
void ANativeWindow_release(ANativeWindow* window);
int32_t ANativeWindow_getWidth(ANativeWindow* window);
int32_t ANativeWindow_getHeight(ANativeWindow* window);
int32_t ANativeWindow_getFormat(ANativeWindow* window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow* window, int32_t w, int32_t h,
                                         int32_t format);
int32_t ANativeWindow_lock(ANativeWindow* window, ANativeWindow_Buffer* buffer, ARect* bounds);
int32_t ANativeWindow_unlockAndPost(ANativeWindow* window);
}

namespace android {

int RegisterGesherNatives(JNIEnv* env, const char* className, const JNINativeMethod* methods,
                          int count);  // services_natives.cc

namespace {

// HAL formats (system/graphics.h): BLOB is JPEG; YCbCr_420_888 (0x23) and
// IMPLEMENTATION_DEFINED (0x22, PRIVATE) are stored as I420.
constexpr int kHalRgba8888 = 1;
constexpr int kHalBlob = 0x21;
constexpr int kBadValue = -22;             // -EINVAL, as SurfaceUtils' BAD_VALUE
// GRALLOC_USAGE_SW_READ_OFTEN | HW_TEXTURE (what a preview consumer has).
constexpr int64_t kUsagePreview = 0x100 | 0x3;

struct Plane {
  size_t offset, size;
  int rowStride, pixelStride;
};

struct Image {
  std::vector<uint8_t> data;
  std::vector<Plane> planes;
  int width = 0, height = 0;
  int64_t timestampNs = 0;
  int dataSpace = 0;
};

struct Reader {
  jobject weakSelf = nullptr;  // global ref to the WeakReference ImageReader gave
  jobject surface = nullptr;   // global ref to its Surface
  int width = 0, height = 0, maxImages = 1, halFormat = 0, dataSpace = 0;
  int64_t usage = 0;
  std::mutex mutex;
  std::deque<Image*> pending;
  int acquired = 0;
  bool closed = false;
};

struct {
  jclass readerClass;
  jfieldID readerContext;
  jmethodID postEvent;
  jfieldID imageBuffer, imageTimestamp, imageDataSpace, imageTransform, imageScalingMode;
  jclass planeClass;
  jmethodID planeCtor;
  jclass surfaceClass;
  jmethodID surfaceCtor;
} gIr;

std::mutex gReadersMutex;
std::vector<Reader*> gReaders;

// The reader whose Surface `surface` is, or null.
Reader* ReaderForSurface(JNIEnv* env, jobject surface) {
  if (surface == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(gReadersMutex);
  for (Reader* r : gReaders) {
    if (r->surface != nullptr && env->IsSameObject(r->surface, surface)) return r;
  }
  return nullptr;
}

Reader* ReaderOf(JNIEnv* env, jobject thiz) {
  return reinterpret_cast<Reader*>(env->GetLongField(thiz, gIr.readerContext));
}

void ImageReader_classInit(JNIEnv* env, jclass clazz) {
  gIr.readerClass = MakeGlobalRefOrDie(env, clazz);
  gIr.readerContext = GetFieldIDOrDie(env, clazz, "mNativeContext", "J");
  gIr.postEvent = GetStaticMethodIDOrDie(env, clazz, "postEventFromNative",
                                         "(Ljava/lang/Object;)V");
  jclass image = FindClassOrDie(env, "android/media/ImageReader$SurfaceImage");
  gIr.imageBuffer = GetFieldIDOrDie(env, image, "mNativeBuffer", "J");
  gIr.imageTimestamp = GetFieldIDOrDie(env, image, "mTimestamp", "J");
  gIr.imageDataSpace = GetFieldIDOrDie(env, image, "mDataSpace", "I");
  gIr.imageTransform = GetFieldIDOrDie(env, image, "mTransform", "I");
  gIr.imageScalingMode = GetFieldIDOrDie(env, image, "mScalingMode", "I");
  gIr.planeClass = MakeGlobalRefOrDie(
      env, FindClassOrDie(env, "android/media/ImageReader$SurfaceImage$SurfacePlane"));
  gIr.planeCtor = GetMethodIDOrDie(env, gIr.planeClass, "<init>",
                                   "(Landroid/media/ImageReader$SurfaceImage;IILjava/nio/ByteBuffer;)V");
  gIr.surfaceClass = MakeGlobalRefOrDie(env, FindClassOrDie(env, "android/view/Surface"));
  gIr.surfaceCtor = GetMethodIDOrDie(env, gIr.surfaceClass, "<init>", "()V");
}

void ImageReader_init(JNIEnv* env, jobject thiz, jobject weakSelf, jint w, jint h, jint maxImages,
                      jlong usage, jint halFormat, jint dataSpace) {
  Reader* r = new Reader();
  r->weakSelf = env->NewGlobalRef(weakSelf);
  r->width = w;
  r->height = h;
  r->maxImages = std::max(maxImages, 1);
  r->halFormat = halFormat;
  r->dataSpace = dataSpace;
  r->usage = usage;
  ScopedLocalRef<jobject> surface(env, env->NewObject(gIr.surfaceClass, gIr.surfaceCtor));
  if (surface.get() == nullptr) {
    env->DeleteGlobalRef(r->weakSelf);
    delete r;
    return;
  }
  r->surface = env->NewGlobalRef(surface.get());
  {
    std::lock_guard<std::mutex> lock(gReadersMutex);
    gReaders.push_back(r);
  }
  env->SetLongField(thiz, gIr.readerContext, reinterpret_cast<jlong>(r));
}

void ImageReader_close(JNIEnv* env, jobject thiz) {
  Reader* r = ReaderOf(env, thiz);
  if (r == nullptr) return;
  env->SetLongField(thiz, gIr.readerContext, 0);
  {
    std::lock_guard<std::mutex> lock(gReadersMutex);
    gReaders.erase(std::remove(gReaders.begin(), gReaders.end(), r), gReaders.end());
  }
  bool gone;
  {
    std::lock_guard<std::mutex> lock(r->mutex);
    for (Image* i : r->pending) delete i;
    r->pending.clear();
    r->closed = true;
    env->DeleteGlobalRef(r->weakSelf);
    env->DeleteGlobalRef(r->surface);
    r->weakSelf = r->surface = nullptr;
    gone = r->acquired <= 0;
  }
  // Images still acquired keep the reader until the last is released.
  if (gone) delete r;
}

Image* ImageOf(JNIEnv* env, jobject image) {
  return reinterpret_cast<Image*>(env->GetLongField(image, gIr.imageBuffer));
}

void ReleaseImage(Reader* r, Image* i) {
  delete i;
  if (r == nullptr) return;
  bool gone;
  {
    std::lock_guard<std::mutex> lock(r->mutex);
    r->acquired--;
    gone = r->closed && r->acquired <= 0;
  }
  if (gone) delete r;
}

void ImageReader_releaseImage(JNIEnv* env, jobject thiz, jobject image) {
  Image* i = ImageOf(env, image);
  if (i == nullptr) return;
  env->SetLongField(image, gIr.imageBuffer, 0);
  ReleaseImage(ReaderOf(env, thiz), i);
}

jint ImageReader_imageSetup(JNIEnv* env, jobject thiz, jobject image) {
  Reader* r = ReaderOf(env, thiz);
  if (r == nullptr) {
    jniThrowException(env, "java/lang/IllegalStateException",
                      "ImageReader is not initialized or was already closed");
    return -1;
  }
  Image* i;
  {
    std::lock_guard<std::mutex> lock(r->mutex);
    if (r->acquired >= r->maxImages) return 2;  // ACQUIRE_MAX_IMAGES
    if (r->pending.empty()) return 1;           // ACQUIRE_NO_BUFS
    i = r->pending.front();
    r->pending.pop_front();
    r->acquired++;
  }
  env->SetLongField(image, gIr.imageBuffer, reinterpret_cast<jlong>(i));
  env->SetLongField(image, gIr.imageTimestamp, i->timestampNs);
  env->SetIntField(image, gIr.imageDataSpace, i->dataSpace);
  env->SetIntField(image, gIr.imageTransform, 0);
  env->SetIntField(image, gIr.imageScalingMode, 0);
  return 0;  // ACQUIRE_SUCCESS
}

jobject ImageReader_getSurface(JNIEnv* env, jobject thiz) {
  Reader* r = ReaderOf(env, thiz);
  return r != nullptr ? env->NewLocalRef(r->surface) : nullptr;
}

jint ImageReader_detachImage(JNIEnv* env, jobject, jobject, jboolean) {
  jniThrowException(env, "java/lang/UnsupportedOperationException",
                    "detaching images is not supported on Gesher");
  return -1;
}

void ImageReader_discardFreeBuffers(JNIEnv*, jobject) {}

jobjectArray ImageReader_createImagePlanes(JNIEnv* env, jclass, jint, jobject, jint, jint, jint,
                                           jint, jint, jint) {
  jniThrowException(env, "java/lang/UnsupportedOperationException",
                    "GraphicBuffer images are not supported on Gesher");
  return nullptr;
}

void ImageReader_unlockGraphicBuffer(JNIEnv* env, jclass, jobject) {
  jniThrowException(env, "java/lang/UnsupportedOperationException",
                    "GraphicBuffer images are not supported on Gesher");
}

jobjectArray Image_createPlanes(JNIEnv* env, jobject thiz, jint numPlanes, jint, jlong) {
  Image* i = ImageOf(env, thiz);
  if (i == nullptr) {
    jniThrowException(env, "java/lang/IllegalStateException", "Image is not initialized");
    return nullptr;
  }
  int n = std::min<int>(numPlanes, static_cast<int>(i->planes.size()));
  jobjectArray planes = env->NewObjectArray(numPlanes, gIr.planeClass, nullptr);
  if (planes == nullptr) return nullptr;
  for (int p = 0; p < n; ++p) {
    const Plane& pl = i->planes[p];
    ScopedLocalRef<jobject> buffer(
        env, env->NewDirectByteBuffer(i->data.data() + pl.offset, static_cast<jlong>(pl.size)));
    ScopedLocalRef<jobject> plane(env, env->NewObject(gIr.planeClass, gIr.planeCtor, thiz,
                                                      pl.rowStride, pl.pixelStride,
                                                      buffer.get()));
    env->SetObjectArrayElement(planes, p, plane.get());
  }
  return planes;
}

jint Image_getWidth(JNIEnv* env, jobject thiz) {
  Image* i = ImageOf(env, thiz);
  return i != nullptr ? i->width : 0;
}

jint Image_getHeight(JNIEnv* env, jobject thiz) {
  Image* i = ImageOf(env, thiz);
  return i != nullptr ? i->height : 0;
}

jint Image_getFormat(JNIEnv*, jobject, jint readerFormat) { return readerFormat; }

jint Image_getFenceFd(JNIEnv*, jobject) { return -1; }

jobject Image_getHardwareBuffer(JNIEnv*, jobject) { return nullptr; }

// --- frames into a reader ----------------------------------------------------------------

// NV12 (w x h) to I420 (tw x th), nearest neighbour.
void Nv12ToI420(const uint8_t* src, int w, int h, uint8_t* y, uint8_t* u, uint8_t* v, int tw,
                int th) {
  const uint8_t* uv = src + static_cast<size_t>(w) * h;
  for (int ty = 0; ty < th; ++ty) {
    int sy = ty * h / th;
    for (int tx = 0; tx < tw; ++tx) y[ty * tw + tx] = src[sy * w + tx * w / tw];
  }
  int cw = (tw + 1) / 2, ch = (th + 1) / 2, scw = w / 2, sch = h / 2;
  for (int cy = 0; cy < ch; ++cy) {
    int sy = std::min(cy * sch / ch, sch - 1);
    for (int cx = 0; cx < cw; ++cx) {
      int sx = std::min(cx * scw / cw, scw - 1);
      u[cy * cw + cx] = uv[sy * w + sx * 2];
      v[cy * cw + cx] = uv[sy * w + sx * 2 + 1];
    }
  }
}

inline uint8_t Clamp(int x) { return static_cast<uint8_t>(x < 0 ? 0 : x > 255 ? 255 : x); }

// NV12 (w x h) to RGBA rows of `stride` pixels (tw x th), BT.601 limited range.
void Nv12ToRgba(const uint8_t* src, int w, int h, uint8_t* dst, int stride, int tw, int th) {
  const uint8_t* uv = src + static_cast<size_t>(w) * h;
  for (int ty = 0; ty < th; ++ty) {
    int sy = ty * h / th;
    uint8_t* row = dst + static_cast<size_t>(ty) * stride * 4;
    for (int tx = 0; tx < tw; ++tx) {
      int sx = tx * w / tw;
      int c = src[sy * w + sx] - 16;
      int d = uv[(sy / 2) * w + (sx / 2) * 2] - 128;
      int e = uv[(sy / 2) * w + (sx / 2) * 2 + 1] - 128;
      row[tx * 4 + 0] = Clamp((298 * c + 409 * e + 128) >> 8);
      row[tx * 4 + 1] = Clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
      row[tx * 4 + 2] = Clamp((298 * c + 516 * d + 128) >> 8);
      row[tx * 4 + 3] = 255;
    }
  }
}

// One frame into a reader: false when the reader cannot take this format.
bool QueueFrame(JNIEnv* env, Reader* r, const uint8_t* nv12, int w, int h, int64_t ts,
                const uint8_t* jpeg, size_t jpegSize) {
  Image* i = new Image();
  i->timestampNs = ts;
  i->dataSpace = r->dataSpace;
  i->width = r->width;
  i->height = r->height;
  int tw = r->width, th = r->height;
  if (r->halFormat == kHalBlob) {
    if (jpeg == nullptr) {
      delete i;
      return false;
    }
    i->data.assign(jpeg, jpeg + jpegSize);
    i->planes.push_back({0, jpegSize, 0, 0});
  } else if (r->halFormat == kHalRgba8888) {
    i->data.resize(static_cast<size_t>(tw) * th * 4);
    Nv12ToRgba(nv12, w, h, i->data.data(), tw, tw, th);
    i->planes.push_back({0, i->data.size(), tw * 4, 4});
  } else {
    // YUV_420_888 (and PRIVATE, whose planes nobody sees): I420.
    size_t ySize = static_cast<size_t>(tw) * th;
    int cw = (tw + 1) / 2, ch = (th + 1) / 2;
    size_t cSize = static_cast<size_t>(cw) * ch;
    i->data.resize(ySize + 2 * cSize);
    Nv12ToI420(nv12, w, h, i->data.data(), i->data.data() + ySize,
               i->data.data() + ySize + cSize, tw, th);
    i->planes.push_back({0, ySize, tw, 1});
    i->planes.push_back({ySize, cSize, cw, 1});
    i->planes.push_back({ySize + cSize, cSize, cw, 1});
  }
  jobject weak;
  {
    std::lock_guard<std::mutex> lock(r->mutex);
    if (r->closed) {
      delete i;
      return true;
    }
    // A consumer that falls behind loses its oldest frames, not the newest.
    while (static_cast<int>(r->pending.size()) >= r->maxImages) {
      delete r->pending.front();
      r->pending.pop_front();
    }
    r->pending.push_back(i);
    weak = env->NewLocalRef(r->weakSelf);
  }
  env->CallStaticVoidMethod(gIr.readerClass, gIr.postEvent, weak);
  env->DeleteLocalRef(weak);
  return true;
}

// CameraFrames.nativeDeliver: 0 delivered, 1 the target cannot take it (no native
// window, or a JPEG reader without JPEG bytes), -1 an error.
gesher::devices::camera::TraceStats gDeliveryTrace{
    "JNI delivery (process cumulative)",
    {"validate", "pin arrays", "lookup target", "target/lock", "convert", "post", "release"}};

jint CameraFrames_deliver(JNIEnv* env, jclass, jobject surface, jbyteArray nv12, jint w, jint h,
                          jlong ts, jbyteArray jpeg) {
  gesher::devices::camera::TraceSample trace(gDeliveryTrace);
  if (surface == nullptr || nv12 == nullptr) return -1;
  jsize size = env->GetArrayLength(nv12);
  if (size < w * h * 3 / 2) return -1;
  trace.Step(0);
  jbyte* frame = env->GetByteArrayElements(nv12, nullptr);
  jbyte* jpegBytes = jpeg != nullptr ? env->GetByteArrayElements(jpeg, nullptr) : nullptr;
  trace.Step(1);
  jint result = 1;
  Reader* r = ReaderForSurface(env, surface);
  trace.Step(2);
  if (r != nullptr) {
    result = QueueFrame(env, r, reinterpret_cast<uint8_t*>(frame), w, h, ts,
                        reinterpret_cast<uint8_t*>(jpegBytes),
                        jpeg != nullptr ? env->GetArrayLength(jpeg) : 0)
                 ? 0
                 : 1;
    trace.Step(3);
  } else {
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    trace.Step(2);
    if (window != nullptr) {
      // A window: drawn at the window's size, RGBA.
      int ww = ANativeWindow_getWidth(window), wh = ANativeWindow_getHeight(window);
      if (ww <= 0 || wh <= 0) {
        ww = w;
        wh = h;
      }
      ANativeWindow_setBuffersGeometry(window, ww, wh, kHalRgba8888);
      ANativeWindow_Buffer buffer;
      int locked = ANativeWindow_lock(window, &buffer, nullptr);
      trace.Step(3);
      if (locked == 0) {
        Nv12ToRgba(reinterpret_cast<uint8_t*>(frame), w, h, static_cast<uint8_t*>(buffer.bits),
                   buffer.stride, std::min(buffer.width, ww), std::min(buffer.height, wh));
        trace.Step(4);
        ANativeWindow_unlockAndPost(window);
        trace.Step(5);
        result = 0;
      }
      ANativeWindow_release(window);
    }
  }
  if (jpegBytes != nullptr) env->ReleaseByteArrayElements(jpeg, jpegBytes, JNI_ABORT);
  env->ReleaseByteArrayElements(nv12, frame, JNI_ABORT);
  trace.Step(6);
  return result;
}

// CameraFrames.nativeToNv21: NV12 (w x h) as NV21 (tw x th), for YuvImage's JPEG encoder.
jbyteArray CameraFrames_toNv21(JNIEnv* env, jclass, jbyteArray nv12, jint w, jint h, jint tw,
                               jint th) {
  if (nv12 == nullptr || env->GetArrayLength(nv12) < w * h * 3 / 2 || tw <= 0 || th <= 0 ||
      tw % 2 != 0 || th % 2 != 0) {
    jniThrowException(env, "java/lang/IllegalArgumentException", "bad frame");
    return nullptr;
  }
  std::vector<uint8_t> out(static_cast<size_t>(tw) * th * 3 / 2);
  size_t ySize = static_cast<size_t>(tw) * th;
  std::vector<uint8_t> u(ySize / 4), v(ySize / 4);
  jbyte* frame = env->GetByteArrayElements(nv12, nullptr);
  Nv12ToI420(reinterpret_cast<uint8_t*>(frame), w, h, out.data(), u.data(), v.data(), tw, th);
  env->ReleaseByteArrayElements(nv12, frame, JNI_ABORT);
  for (size_t i = 0; i < u.size(); ++i) {
    out[ySize + 2 * i] = v[i];
    out[ySize + 2 * i + 1] = u[i];
  }
  jbyteArray result = env->NewByteArray(static_cast<jsize>(out.size()));
  if (result != nullptr) {
    env->SetByteArrayRegion(result, 0, static_cast<jsize>(out.size()),
                            reinterpret_cast<const jbyte*>(out.data()));
  }
  return result;
}

// CameraFrames.nativeIsReader: whether `surface` is an ImageReader's (Gesher's).
jboolean CameraFrames_isReader(JNIEnv* env, jclass, jobject surface) {
  return ReaderForSurface(env, surface) != nullptr;
}

// --- SurfaceUtils ----------------------------------------------------------------------

jint SurfaceUtils_detectSurfaceType(JNIEnv* env, jclass, jobject surface) {
  if (Reader* r = ReaderForSurface(env, surface)) return r->halFormat;
  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (window == nullptr) return kBadValue;
  int format = ANativeWindow_getFormat(window);
  ANativeWindow_release(window);
  return format > 0 ? format : kHalRgba8888;
}

jint SurfaceUtils_detectSurfaceDataspace(JNIEnv* env, jclass, jobject surface) {
  if (Reader* r = ReaderForSurface(env, surface)) return r->dataSpace;
  return 0;
}

jint SurfaceUtils_detectSurfaceDimens(JNIEnv* env, jclass, jobject surface, jintArray dimens) {
  jint wh[2] = {0, 0};
  if (Reader* r = ReaderForSurface(env, surface)) {
    wh[0] = r->width;
    wh[1] = r->height;
  } else if (ANativeWindow* window = ANativeWindow_fromSurface(env, surface)) {
    wh[0] = ANativeWindow_getWidth(window);
    wh[1] = ANativeWindow_getHeight(window);
    ANativeWindow_release(window);
  } else {
    return kBadValue;
  }
  if (dimens == nullptr || env->GetArrayLength(dimens) < 2) return kBadValue;
  env->SetIntArrayRegion(dimens, 0, 2, wh);
  return 0;
}

jlong SurfaceUtils_detectSurfaceUsageFlags(JNIEnv* env, jclass, jobject surface) {
  if (Reader* r = ReaderForSurface(env, surface)) return r->usage;
  return kUsagePreview;
}

jlong SurfaceUtils_getSurfaceId(JNIEnv* env, jclass, jobject surface) {
  if (Reader* r = ReaderForSurface(env, surface)) return reinterpret_cast<jlong>(r);
  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (window == nullptr) return 0;
  ANativeWindow_release(window);
  return reinterpret_cast<jlong>(window);
}

#define SURFACE "Landroid/view/Surface;"

const JNINativeMethod gImageReaderMethods[] = {
    {"nativeClassInit", "()V", GESHER_JNI(ImageReader_classInit)},
    {"nativeInit", "(Ljava/lang/Object;IIIJII)V", GESHER_JNI(ImageReader_init)},
    {"nativeClose", "()V", GESHER_JNI(ImageReader_close)},
    {"nativeReleaseImage", "(Landroid/media/Image;)V", GESHER_JNI(ImageReader_releaseImage)},
    {"nativeImageSetup", "(Landroid/media/Image;)I", GESHER_JNI(ImageReader_imageSetup)},
    {"nativeGetSurface", "()" SURFACE, GESHER_JNI(ImageReader_getSurface)},
    {"nativeDetachImage", "(Landroid/media/Image;Z)I", GESHER_JNI(ImageReader_detachImage)},
    {"nativeCreateImagePlanes",
     "(ILandroid/graphics/GraphicBuffer;IIIIII)[Landroid/media/ImageReader$ImagePlane;",
     GESHER_JNI(ImageReader_createImagePlanes)},
    {"nativeUnlockGraphicBuffer", "(Landroid/graphics/GraphicBuffer;)V",
     GESHER_JNI(ImageReader_unlockGraphicBuffer)},
    {"nativeDiscardFreeBuffers", "()V", GESHER_JNI(ImageReader_discardFreeBuffers)},
};

const JNINativeMethod gSurfaceImageMethods[] = {
    {"nativeCreatePlanes", "(IIJ)[Landroid/media/ImageReader$SurfaceImage$SurfacePlane;",
     GESHER_JNI(Image_createPlanes)},
    {"nativeGetWidth", "()I", GESHER_JNI(Image_getWidth)},
    {"nativeGetHeight", "()I", GESHER_JNI(Image_getHeight)},
    {"nativeGetFormat", "(I)I", GESHER_JNI(Image_getFormat)},
    {"nativeGetFenceFd", "()I", GESHER_JNI(Image_getFenceFd)},
    {"nativeGetHardwareBuffer", "()Landroid/hardware/HardwareBuffer;",
     GESHER_JNI(Image_getHardwareBuffer)},
};

const JNINativeMethod gSurfaceUtilsMethods[] = {
    {"nativeDetectSurfaceType", "(" SURFACE ")I", GESHER_JNI(SurfaceUtils_detectSurfaceType)},
    {"nativeDetectSurfaceDataspace", "(" SURFACE ")I",
     GESHER_JNI(SurfaceUtils_detectSurfaceDataspace)},
    {"nativeDetectSurfaceDimens", "(" SURFACE "[I)I", GESHER_JNI(SurfaceUtils_detectSurfaceDimens)},
    {"nativeDetectSurfaceUsageFlags", "(" SURFACE ")J",
     GESHER_JNI(SurfaceUtils_detectSurfaceUsageFlags)},
    {"nativeGetSurfaceId", "(" SURFACE ")J", GESHER_JNI(SurfaceUtils_getSurfaceId)},
};

const JNINativeMethod gCameraFramesMethods[] = {
    {"nativeDeliver", "(" SURFACE "[BIIJ[B)I", GESHER_JNI(CameraFrames_deliver)},
    {"nativeIsReader", "(" SURFACE ")Z", GESHER_JNI(CameraFrames_isReader)},
    {"nativeToNv21", "([BIIII)[B", GESHER_JNI(CameraFrames_toNv21)},
};

// --- android.media.PublicFormatUtils (frameworks/base's android_media_PublicFormatUtils
// over libui's PublicFormat.cpp, which Gesher does not build): ImageFormat <-> HAL
// format and dataspace, as ui/PublicFormat.cpp maps them.

namespace pf {
constexpr int kJpeg = 0x100, kDepthPointCloud = 0x101, kDepthJpeg = 0x69656963,
              kHeic = 0x48454946, kJpegR = 0x1005, kDepth16 = 0x44363159, kRawSensor = 0x20,
              kRawPrivate = 0x24, kRaw10 = 0x25, kRaw12 = 0x26, kRawDepth = 0x1002,
              kRawDepth10 = 0x1003, kYuv420 = 0x23, kNv21 = 0x11, kYv12 = 0x32315659;
constexpr int kHalY16 = 0x20363159, kHalRaw16 = 0x20, kHalRaw10 = 0x25;
constexpr int kDsUnknown = 0, kDsArbitrary = 1, kDsDepth = 0x1000, kDsDynamicDepth = 0x1002,
              kDsHeif = 0x1004, kDsJpegR = 0x1005, kDsJfif = 0x8C20000;
}  // namespace pf

jint PublicFormat_getHalFormat(JNIEnv*, jclass, jint format) {
  switch (format) {
    case pf::kJpeg:
    case pf::kDepthPointCloud:
    case pf::kDepthJpeg:
    case pf::kHeic:
    case pf::kJpegR:
      return kHalBlob;
    case pf::kDepth16:
      return pf::kHalY16;
    case pf::kRawSensor:
    case pf::kRawDepth:
      return pf::kHalRaw16;
    case pf::kRawDepth10:
      return pf::kHalRaw10;
    default:
      return format;  // the other public formats are their HAL formats
  }
}

jint PublicFormat_getHalDataspace(JNIEnv*, jclass, jint format) {
  switch (format) {
    case pf::kDepthPointCloud:
    case pf::kDepth16:
    case pf::kRawDepth:
    case pf::kRawDepth10:
      return pf::kDsDepth;
    case pf::kRawSensor:
    case pf::kRawPrivate:
    case pf::kRaw10:
    case pf::kRaw12:
      return pf::kDsArbitrary;
    case pf::kYuv420:
    case pf::kNv21:
    case pf::kYv12:
    case pf::kJpeg:
      return pf::kDsJfif;
    case pf::kDepthJpeg:
      return pf::kDsDynamicDepth;
    case pf::kHeic:
      return pf::kDsHeif;
    case pf::kJpegR:
      return pf::kDsJpegR;
    default:
      return pf::kDsUnknown;
  }
}

jint PublicFormat_getPublicFormat(JNIEnv*, jclass, jint halFormat, jint dataspace) {
  switch (halFormat) {
    case kHalBlob:
      switch (dataspace) {
        case pf::kDsDepth: return pf::kDepthPointCloud;
        case pf::kDsDynamicDepth: return pf::kDepthJpeg;
        case pf::kDsHeif: return pf::kHeic;
        case pf::kDsJpegR: return pf::kJpegR;
        default: return pf::kJpeg;
      }
    case pf::kHalY16:
      return dataspace == pf::kDsDepth ? pf::kDepth16 : halFormat;
    case pf::kHalRaw16:
      return dataspace == pf::kDsDepth ? pf::kRawDepth : pf::kRawSensor;
    case pf::kHalRaw10:
      return dataspace == pf::kDsDepth ? pf::kRawDepth10 : pf::kRaw10;
    default:
      return halFormat;
  }
}

const JNINativeMethod gPublicFormatUtilsMethods[] = {
    {"nativeGetHalFormat", "(I)I", GESHER_JNI(PublicFormat_getHalFormat)},
    {"nativeGetHalDataspace", "(I)I", GESHER_JNI(PublicFormat_getHalDataspace)},
    {"nativeGetPublicFormat", "(II)I", GESHER_JNI(PublicFormat_getPublicFormat)},
};

// --- android.hardware.Camera (the legacy API) --------------------------------------------
// Over the same devices as Camera2 (hwsvc2 track, ADR 0051): each native calls
// com.gesher.server.LegacyCamera (gesher-app.jar, on the boot class path) with the
// Camera; its exceptions reach the app as the natives' would.

jclass LegacyClass(JNIEnv* env) {
  static jclass cls = nullptr;
  if (cls == nullptr) {
    jclass local = env->FindClass("com/gesher/server/LegacyCamera");
    if (local == nullptr) return nullptr;
    cls = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
  }
  return cls;
}

jmethodID LegacyMethod(JNIEnv* env, const char* name, const char* sig) {
  jclass cls = LegacyClass(env);
  return cls != nullptr ? env->GetStaticMethodID(cls, name, sig) : nullptr;
}

#define LEGACY_CAMERA "Landroid/hardware/Camera;"

// A void call with the Camera and up to two more arguments.
template <typename... Args>
void LegacyVoid(JNIEnv* env, const char* name, const char* sig, jobject thiz, Args... args) {
  jmethodID m = LegacyMethod(env, name, sig);
  if (m != nullptr) env->CallStaticVoidMethod(LegacyClass(env), m, thiz, args...);
}

jint LegacyCamera_getNumberOfCameras(JNIEnv* env, jclass, jobject, jint) {
  jmethodID m = LegacyMethod(env, "count", "()I");
  return m != nullptr ? env->CallStaticIntMethod(LegacyClass(env), m) : 0;
}

void LegacyCamera_getCameraInfo(JNIEnv* env, jclass, jint id, jobject, jobject, jint,
                                jobject info) {
  jmethodID m = LegacyMethod(env, "info", "(ILandroid/hardware/Camera$CameraInfo;)V");
  if (m != nullptr) env->CallStaticVoidMethod(LegacyClass(env), m, id, info);
}

jint LegacyCamera_setup(JNIEnv* env, jobject thiz, jobject weakThis, jint id, jobject, jboolean,
                        jobject, jint) {
  jmethodID m = LegacyMethod(env, "setup", "(" LEGACY_CAMERA "Ljava/lang/Object;I)I");
  return m != nullptr ? env->CallStaticIntMethod(LegacyClass(env), m, thiz, weakThis, id)
                      : -19;  // -ENODEV
}

void LegacyCamera_release(JNIEnv* env, jobject thiz) {
  LegacyVoid(env, "release", "(" LEGACY_CAMERA ")V", thiz);
}

// unlock, lock, reconnect, cancelAutoFocus, stopSmoothZoom, stopFaceDetection,
// setDisplayOrientation, enableFocusMoveCallback, setPreviewCallbackSurface: nothing to
// do for an open camera (one process, fixed focus, frames are not rotated).
void LegacyCamera_check(JNIEnv* env, jobject thiz) {
  LegacyVoid(env, "check", "(" LEGACY_CAMERA ")V", thiz);
}

void LegacyCamera_checkInt(JNIEnv* env, jobject thiz, jint) { LegacyCamera_check(env, thiz); }

void LegacyCamera_checkObject(JNIEnv* env, jobject thiz, jobject) {
  LegacyCamera_check(env, thiz);
}

void LegacyCamera_setPreviewSurface(JNIEnv* env, jobject thiz, jobject surface) {
  LegacyVoid(env, "setPreviewSurface", "(" LEGACY_CAMERA "Landroid/view/Surface;)V", thiz,
             surface);
}

void LegacyCamera_setPreviewTexture(JNIEnv* env, jobject thiz, jobject texture) {
  LegacyVoid(env, "setPreviewTexture", "(" LEGACY_CAMERA "Ljava/lang/Object;)V", thiz, texture);
}

void LegacyCamera_startPreview(JNIEnv* env, jobject thiz) {
  LegacyVoid(env, "startPreview", "(" LEGACY_CAMERA ")V", thiz);
}

void LegacyCamera_stopPreview(JNIEnv* env, jobject thiz) {
  LegacyVoid(env, "stopPreview", "(" LEGACY_CAMERA ")V", thiz);
}

jboolean LegacyCamera_previewEnabled(JNIEnv* env, jobject thiz) {
  jmethodID m = LegacyMethod(env, "previewEnabled", "(" LEGACY_CAMERA ")Z");
  return m != nullptr ? env->CallStaticBooleanMethod(LegacyClass(env), m, thiz) : JNI_FALSE;
}

void LegacyCamera_setHasPreviewCallback(JNIEnv* env, jobject thiz, jboolean installed,
                                        jboolean manual) {
  LegacyVoid(env, "setHasPreviewCallback", "(" LEGACY_CAMERA "ZZ)V", thiz, installed, manual);
}

void LegacyCamera_addCallbackBuffer(JNIEnv* env, jobject thiz, jbyteArray buffer, jint type) {
  LegacyVoid(env, "addCallbackBuffer", "(" LEGACY_CAMERA "[BI)V", thiz, buffer, type);
}

void LegacyCamera_autoFocus(JNIEnv* env, jobject thiz) {
  LegacyVoid(env, "autoFocus", "(" LEGACY_CAMERA ")V", thiz);
}

void LegacyCamera_takePicture(JNIEnv* env, jobject thiz, jint msgType) {
  LegacyVoid(env, "takePicture", "(" LEGACY_CAMERA "I)V", thiz, msgType);
}

void LegacyCamera_startSmoothZoom(JNIEnv* env, jobject thiz, jint value) {
  LegacyVoid(env, "startSmoothZoom", "(" LEGACY_CAMERA "I)V", thiz, value);
}

void LegacyCamera_startFaceDetection(JNIEnv* env, jobject thiz, jint type) {
  LegacyVoid(env, "startFaceDetection", "(" LEGACY_CAMERA "I)V", thiz, type);
}

jboolean LegacyCamera_enableShutterSound(JNIEnv* env, jobject thiz, jboolean) {
  LegacyCamera_check(env, thiz);
  return JNI_TRUE;
}

void LegacyCamera_setParameters(JNIEnv* env, jobject thiz, jstring params) {
  LegacyVoid(env, "setParameters", "(" LEGACY_CAMERA "Ljava/lang/String;)V", thiz, params);
}

jstring LegacyCamera_getParameters(JNIEnv* env, jobject thiz) {
  jmethodID m = LegacyMethod(env, "getParameters", "(" LEGACY_CAMERA ")Ljava/lang/String;");
  return m != nullptr
             ? static_cast<jstring>(env->CallStaticObjectMethod(LegacyClass(env), m, thiz))
             : nullptr;
}

void LegacyCamera_setAudioRestriction(JNIEnv* env, jobject thiz, jint mode) {
  LegacyVoid(env, "setAudioRestriction", "(" LEGACY_CAMERA "I)V", thiz, mode);
}

jint LegacyCamera_getAudioRestriction(JNIEnv* env, jobject thiz) {
  jmethodID m = LegacyMethod(env, "getAudioRestriction", "(" LEGACY_CAMERA ")I");
  return m != nullptr ? env->CallStaticIntMethod(LegacyClass(env), m, thiz) : 0;
}

const JNINativeMethod gLegacyCameraMethods[] = {
    {"_getNumberOfCameras", "(Landroid/os/Parcel;I)I", GESHER_JNI(LegacyCamera_getNumberOfCameras)},
    {"_getCameraInfo",
     "(ILandroid/content/res/CameraCompatibilityInfo;Landroid/os/Parcel;ILandroid/hardware/"
     "Camera$CameraInfo;)V",
     GESHER_JNI(LegacyCamera_getCameraInfo)},
    {"native_setup",
     "(Ljava/lang/Object;ILandroid/content/res/CameraCompatibilityInfo;ZLandroid/os/Parcel;I)I",
     GESHER_JNI(LegacyCamera_setup)},
    {"native_release", "()V", GESHER_JNI(LegacyCamera_release)},
    {"unlock", "()V", GESHER_JNI(LegacyCamera_check)},
    {"lock", "()V", GESHER_JNI(LegacyCamera_check)},
    {"reconnect", "()V", GESHER_JNI(LegacyCamera_check)},
    {"setPreviewSurface", "(Landroid/view/Surface;)V", GESHER_JNI(LegacyCamera_setPreviewSurface)},
    {"setPreviewTexture", "(Landroid/graphics/SurfaceTexture;)V",
     GESHER_JNI(LegacyCamera_setPreviewTexture)},
    {"startPreview", "()V", GESHER_JNI(LegacyCamera_startPreview)},
    {"_stopPreview", "()V", GESHER_JNI(LegacyCamera_stopPreview)},
    {"previewEnabled", "()Z", GESHER_JNI(LegacyCamera_previewEnabled)},
    {"setHasPreviewCallback", "(ZZ)V", GESHER_JNI(LegacyCamera_setHasPreviewCallback)},
    {"_addCallbackBuffer", "([BI)V", GESHER_JNI(LegacyCamera_addCallbackBuffer)},
    {"setPreviewCallbackSurface", "(Landroid/view/Surface;)V",
     GESHER_JNI(LegacyCamera_checkObject)},
    {"native_autoFocus", "()V", GESHER_JNI(LegacyCamera_autoFocus)},
    {"native_cancelAutoFocus", "()V", GESHER_JNI(LegacyCamera_check)},
    {"enableFocusMoveCallback", "(I)V", GESHER_JNI(LegacyCamera_checkInt)},
    {"native_takePicture", "(I)V", GESHER_JNI(LegacyCamera_takePicture)},
    {"startSmoothZoom", "(I)V", GESHER_JNI(LegacyCamera_startSmoothZoom)},
    {"stopSmoothZoom", "()V", GESHER_JNI(LegacyCamera_check)},
    {"setDisplayOrientation", "(I)V", GESHER_JNI(LegacyCamera_checkInt)},
    {"_enableShutterSound", "(Z)Z", GESHER_JNI(LegacyCamera_enableShutterSound)},
    {"_startFaceDetection", "(I)V", GESHER_JNI(LegacyCamera_startFaceDetection)},
    {"_stopFaceDetection", "()V", GESHER_JNI(LegacyCamera_check)},
    {"native_setParameters", "(Ljava/lang/String;)V", GESHER_JNI(LegacyCamera_setParameters)},
    {"native_getParameters", "()Ljava/lang/String;", GESHER_JNI(LegacyCamera_getParameters)},
    {"setAudioRestriction", "(I)V", GESHER_JNI(LegacyCamera_setAudioRestriction)},
    {"getAudioRestriction", "()I", GESHER_JNI(LegacyCamera_getAudioRestriction)},
};
// ImageReader's static initializer loads libmedia_jni.so: when no other part of
// Gesher provides it, an empty one does (the natives are registered here).
jint MediaJniOnLoad(JavaVM*, void*) { return JNI_VERSION_1_6; }

const gesher_builtin_symbol kMediaJni[] = {
    {"JNI_OnLoad", reinterpret_cast<void*>(&MediaJniOnLoad)},
};

}  // namespace

void LogCameraDeliveryTrace() {
  gDeliveryTrace.Log();
}

int register_gesher_camera_frames(JNIEnv* env) {
  RegisterGesherNatives(env, "android/media/ImageReader", gImageReaderMethods,
                        NELEM(gImageReaderMethods));
  RegisterGesherNatives(env, "android/media/ImageReader$SurfaceImage", gSurfaceImageMethods,
                        NELEM(gSurfaceImageMethods));
  RegisterGesherNatives(env, "android/hardware/camera2/utils/SurfaceUtils", gSurfaceUtilsMethods,
                        NELEM(gSurfaceUtilsMethods));
  RegisterGesherNatives(env, "com/gesher/server/CameraFrames", gCameraFramesMethods,
                        NELEM(gCameraFramesMethods));
  RegisterGesherNatives(env, "android/media/PublicFormatUtils", gPublicFormatUtilsMethods,
                        NELEM(gPublicFormatUtilsMethods));
  RegisterGesherNatives(env, "android/hardware/Camera", gLegacyCameraMethods,
                        NELEM(gLegacyCameraMethods));
  if (dlopen("libmedia_jni.so", RTLD_NOW) == nullptr) {
    gesher_register_builtin_library("libmedia_jni.so", kMediaJni,
                                    sizeof(kMediaJni) / sizeof(kMediaJni[0]));
  }
  return 0;
}

}  // namespace android
