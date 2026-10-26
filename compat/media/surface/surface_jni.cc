// Gesher media: surfaces whose frames hwui draws (docs/media.md "Video",
// ADR 0051): TextureView's SurfaceTexture and SurfaceView's surface, without
// a compositor.
//
//  - android.graphics.SurfaceTexture: a QueueSurface (queue_surface.h) for its
//    producer; each frame calls SurfaceTexture.postEventFromNative, so the
//    TextureView updates its layer and invalidates.
//  - TextureView's layer: HardwareRenderer.nCreateTextureLayer makes a
//    Gesher TextureLayer (not hwui's DeferredLayerUpdater, which needs the
//    device's ASurfaceTexture), TextureLayer's natives configure it, and
//    RecordingCanvas.nDrawTextureLayer records a drawable that draws the
//    SurfaceTexture's latest frame when the display list plays back, on the
//    CPU pipeline and on SkiaGL alike (a raster image).
//  - SurfaceView: its BLASTBufferQueue's Surface is a QueueSurface; the
//    SurfaceView draws the latest frame into the hole it punches
//    (frameworks/base patch 0300: SurfaceView.nativeGesherDrawSurface) and is
//    invalidated by each frame.
//  - android.view.Surface's nativeCreateFromSurfaceTexture and
//    nativeGetFromBlastBufferQueue, which the host's android_view_Surface.cpp
//    leaves out.
//
// Registered from register_gesher_media (after hwui's and the device
// stand-ins' tables, which these replace in part). Compiled into hwui.

#include <jni.h>

#include <SkBitmap.h>
#include <SkCanvas.h>
#include <SkColorFilter.h>
#include <SkData.h>
#include <SkDrawable.h>
#include <SkImage.h>
#include <SkMatrix.h>
#include <SkPaint.h>
#include <SkRRect.h>
#include <SkSamplingOptions.h>

#include <GraphicsJNI.h>
#include <SkiaCanvas.h>
#include <android/graphics/canvas.h>
#include <android/native_window.h>
#include <android_runtime/android_view_Surface.h>
#include <hwui/Bitmap.h>
#include <hwui/Canvas.h>
#include <hwui/Paint.h>
#include <log/log.h>
#include <utils/LightRefBase.h>

#include <map>
#include <memory>
#include <mutex>

#include "queue_surface.h"

#define SYSV __attribute__((sysv_abi))

// The device stand-ins' BLASTBufferQueue natives (compat/framework/device_standins.cc).
extern "C" {
SYSV jobject gesher_device_Blast_getSurface(JNIEnv* env, jobject, jlong ptr, jboolean handle);
SYSV void gesher_device_Blast_update(JNIEnv* env, jobject, jlong ptr, jlong surface_control,
                                     jlong width, jlong height, jint format);
}

using gesher::media::QueueSurface;
using gesher::media::SurfaceFrame;

namespace {

JavaVM* gVm = nullptr;

JNIEnv* Env() {
  JNIEnv* env = nullptr;
  if (gVm == nullptr) return nullptr;
  if (gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
  JavaVMAttachArgs args = {JNI_VERSION_1_6, const_cast<char*>("gesher-surface"), nullptr};
  if (gVm->AttachCurrentThreadAsDaemon(&env, &args) != JNI_OK) return nullptr;
  // Detached when the thread ends (ART aborts on a native thread that exits attached).
  struct Detacher {
    JavaVM* vm = nullptr;
    ~Detacher() {
      if (vm != nullptr) vm->DetachCurrentThread();
    }
  };
  static thread_local Detacher detacher;
  detacher.vm = gVm;
  return env;
}

// The latest frame of a surface as an image (made once per frame).
class FrameImages {
 public:
  sk_sp<SkImage> Get(const std::shared_ptr<const SurfaceFrame>& f) {
    if (f == nullptr) return nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    if (image_ != nullptr && number_ == f->number) return image_;
    auto* keep = new std::shared_ptr<const SurfaceFrame>(f);
    sk_sp<SkData> data = SkData::MakeWithProc(
        f->pixels.data(), f->pixels.size() * 4,
        [](const void*, void* context) { delete static_cast<std::shared_ptr<const SurfaceFrame>*>(context); },
        keep);
    SkImageInfo info = SkImageInfo::Make(f->width, f->height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    image_ = SkImages::RasterFromData(info, std::move(data), static_cast<size_t>(f->width) * 4);
    number_ = f->number;
    return image_;
  }

 private:
  std::mutex mu_;
  sk_sp<SkImage> image_;
  uint64_t number_ = 0;
};

// --- SurfaceTexture -------------------------------------------------------------------------

struct SurfaceTextureState {
  android::sp<QueueSurface> surface;
  jobject weak_self = nullptr;  // global ref to the WeakReference
  int64_t timestamp_ns = 0;
  bool released = false;
};

struct {
  jfieldID surface_texture;  // SurfaceTexture.mSurfaceTexture
  jmethodID post_event;      // SurfaceTexture.postEventFromNative(WeakReference)
  jclass surface_texture_class;
  jmethodID post_invalidate;  // View.postInvalidate()
} gST;

SurfaceTextureState* StateOf(JNIEnv* env, jobject thiz) {
  return reinterpret_cast<SurfaceTextureState*>(env->GetLongField(thiz, gST.surface_texture));
}

void JNICALL ST_init(JNIEnv* env, jobject thiz, jboolean, jint, jboolean, jobject weak_self) {
  auto* s = new SurfaceTextureState();
  s->surface = android::sp<QueueSurface>::make(1, 1);
  s->weak_self = env->NewGlobalRef(weak_self);
  jobject weak = s->weak_self;
  s->surface->SetListener([weak] {
    JNIEnv* e = Env();
    if (e == nullptr) return;
    e->CallStaticVoidMethod(gST.surface_texture_class, gST.post_event, weak);
    if (e->ExceptionCheck()) e->ExceptionClear();
  });
  env->SetLongField(thiz, gST.surface_texture, reinterpret_cast<jlong>(s));
}

void ReleaseState(JNIEnv* env, SurfaceTextureState* s) {
  if (s == nullptr || s->released) return;
  s->released = true;
  s->surface->Abandon();
}

void JNICALL ST_release(JNIEnv* env, jobject thiz) { ReleaseState(env, StateOf(env, thiz)); }

void JNICALL ST_finalize(JNIEnv* env, jobject thiz) {
  SurfaceTextureState* s = StateOf(env, thiz);
  if (s == nullptr) return;
  ReleaseState(env, s);
  env->SetLongField(thiz, gST.surface_texture, 0);
  env->DeleteGlobalRef(s->weak_self);
  delete s;
}

jboolean JNICALL ST_isReleased(JNIEnv* env, jobject thiz) {
  SurfaceTextureState* s = StateOf(env, thiz);
  return s == nullptr || s->released ? JNI_TRUE : JNI_FALSE;
}

void JNICALL ST_setDefaultBufferSize(JNIEnv* env, jobject thiz, jint w, jint h) {
  if (SurfaceTextureState* s = StateOf(env, thiz)) s->surface->SetDefaultSize(w, h);
}

// No GL texture behind it: latching a frame only takes its time stamp (apps that
// sample the external texture themselves get nothing new).
void JNICALL ST_updateTexImage(JNIEnv* env, jobject thiz) {
  SurfaceTextureState* s = StateOf(env, thiz);
  if (s == nullptr) return;
  auto f = s->surface->Latest();
  if (f != nullptr) s->timestamp_ns = f->timestamp_ns;
}

void JNICALL ST_releaseTexImage(JNIEnv*, jobject) {}

jlong JNICALL ST_getTimestamp(JNIEnv* env, jobject thiz) {
  SurfaceTextureState* s = StateOf(env, thiz);
  return s != nullptr ? s->timestamp_ns : 0;
}

jint JNICALL ST_getDataSpace(JNIEnv*, jobject) { return 0; }

// The usual transform of a buffer for GL's bottom-up texture coordinates.
void JNICALL ST_getTransformMatrix(JNIEnv* env, jobject, jfloatArray mtx) {
  const jfloat m[16] = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1};
  if (mtx != nullptr && env->GetArrayLength(mtx) >= 16) env->SetFloatArrayRegion(mtx, 0, 16, m);
}

jint JNICALL ST_detachFromGLContext(JNIEnv*, jobject) { return 0; }
jint JNICALL ST_attachToGLContext(JNIEnv*, jobject, jint) { return 0; }

// Surface.nativeCreateFromSurfaceTexture(SurfaceTexture): its producer side.
jlong JNICALL Surface_createFromSurfaceTexture(JNIEnv* env, jclass, jobject surface_texture) {
  SurfaceTextureState* s = surface_texture != nullptr ? StateOf(env, surface_texture) : nullptr;
  if (s == nullptr || s->released) {
    jniThrowException(env, "java/lang/IllegalArgumentException",
                      "SurfaceTexture has already been released");
    return 0;
  }
  s->surface->incStrong(reinterpret_cast<void*>(&Surface_createFromSurfaceTexture));
  return reinterpret_cast<jlong>(static_cast<android::Surface*>(s->surface.get()));
}

// TextureView.nCreateNativeWindow(SurfaceTexture) / nDestroyNativeWindow(): the window
// TextureView.lockCanvas would draw into (TextureView.mNativeWindow).
jfieldID gTextureViewNativeWindow;

void JNICALL TV_createNativeWindow(JNIEnv* env, jobject thiz, jobject surface_texture) {
  if (gTextureViewNativeWindow == nullptr) {
    jclass c = env->GetObjectClass(thiz);
    gTextureViewNativeWindow = env->GetFieldID(c, "mNativeWindow", "J");
    env->DeleteLocalRef(c);
    if (gTextureViewNativeWindow == nullptr) {
      env->ExceptionClear();
      return;
    }
  }
  SurfaceTextureState* s = surface_texture != nullptr ? StateOf(env, surface_texture) : nullptr;
  if (s == nullptr) return;
  s->surface->incStrong(reinterpret_cast<void*>(&TV_createNativeWindow));
  env->SetLongField(thiz, gTextureViewNativeWindow,
                    reinterpret_cast<jlong>(static_cast<ANativeWindow*>(s->surface.get())));
}

void JNICALL TV_destroyNativeWindow(JNIEnv* env, jobject thiz) {
  if (gTextureViewNativeWindow == nullptr) return;
  auto* w = reinterpret_cast<ANativeWindow*>(env->GetLongField(thiz, gTextureViewNativeWindow));
  env->SetLongField(thiz, gTextureViewNativeWindow, 0);
  if (QueueSurface* q = QueueSurface::From(w)) q->decStrong(reinterpret_cast<void*>(&TV_createNativeWindow));
}

// TextureView.lockCanvas / unlockCanvasAndPost (surfaces track): the Canvas draws into the
// SurfaceTexture's producer (its QueueSurface), as core/jni/android_view_TextureView.cpp
// does on a device; the frame posted becomes the layer's latest one.
jboolean JNICALL TV_lockCanvas(JNIEnv* env, jclass, jlong native_window, jobject canvas_obj,
                               jobject dirty) {
  auto* window = reinterpret_cast<ANativeWindow*>(native_window);
  if (window == nullptr) return JNI_FALSE;
  jclass rect_class = env->FindClass("android/graphics/Rect");
  jfieldID left = env->GetFieldID(rect_class, "left", "I");
  jfieldID top = env->GetFieldID(rect_class, "top", "I");
  jfieldID right = env->GetFieldID(rect_class, "right", "I");
  jfieldID bottom = env->GetFieldID(rect_class, "bottom", "I");
  ARect rect = {0, 0, 0x3FFF, 0x3FFF};
  if (dirty != nullptr) {
    rect = {env->GetIntField(dirty, left), env->GetIntField(dirty, top),
            env->GetIntField(dirty, right), env->GetIntField(dirty, bottom)};
  }
  ANativeWindow_Buffer buffer;
  if (ANativeWindow_lock(window, &buffer, &rect) != 0) {
    env->DeleteLocalRef(rect_class);
    return JNI_FALSE;
  }
  // Within the buffer (libgui's Surface::lock clips the dirty region the same way).
  if (rect.left < 0) rect.left = 0;
  if (rect.top < 0) rect.top = 0;
  if (rect.right > buffer.width) rect.right = buffer.width;
  if (rect.bottom > buffer.height) rect.bottom = buffer.height;
  graphics::Canvas canvas(env, canvas_obj);
  canvas.setBuffer(&buffer, ADATASPACE_UNKNOWN);
  canvas.clipRect({rect.left, rect.top, rect.right, rect.bottom});
  if (dirty != nullptr) {
    env->SetIntField(dirty, left, rect.left);
    env->SetIntField(dirty, top, rect.top);
    env->SetIntField(dirty, right, rect.right);
    env->SetIntField(dirty, bottom, rect.bottom);
  }
  env->DeleteLocalRef(rect_class);
  return JNI_TRUE;
}

void JNICALL TV_unlockCanvasAndPost(JNIEnv* env, jclass, jlong native_window, jobject canvas_obj) {
  graphics::Canvas canvas(env, canvas_obj);
  canvas.setBuffer(nullptr, ADATASPACE_UNKNOWN);
  if (auto* window = reinterpret_cast<ANativeWindow*>(native_window)) {
    ANativeWindow_unlockAndPost(window);
  }
}

// --- TextureView's layer --------------------------------------------------------------------

class TextureLayer : public android::VirtualLightRefBase {
 public:
  std::mutex mu;
  android::sp<QueueSurface> surface;
  int width = 0, height = 0;
  bool blend = false;
  SkMatrix transform = SkMatrix::I();
  int alpha = 255;
  SkBlendMode mode = SkBlendMode::kSrcOver;
  sk_sp<SkColorFilter> filter;
  FrameImages images;

  sk_sp<SkImage> Image() {
    android::sp<QueueSurface> s;
    {
      std::lock_guard<std::mutex> lock(mu);
      s = surface;
    }
    return s != nullptr ? images.Get(s->Latest()) : nullptr;
  }
};

class TextureLayerDrawable : public SkDrawable {
 public:
  explicit TextureLayerDrawable(TextureLayer* layer) : layer_(layer) {}

 protected:
  SkRect onGetBounds() override {
    std::lock_guard<std::mutex> lock(layer_->mu);
    return SkRect::MakeIWH(layer_->width, layer_->height);
  }

  void onDraw(SkCanvas* canvas) override {
    sk_sp<SkImage> image = layer_->Image();
    if (image == nullptr) return;
    SkPaint paint;
    SkMatrix m;
    int w, h;
    {
      std::lock_guard<std::mutex> lock(layer_->mu);
      paint.setAlpha(static_cast<U8CPU>(layer_->alpha));
      paint.setBlendMode(layer_->mode);
      paint.setColorFilter(layer_->filter);
      m = layer_->transform;
      w = layer_->width;
      h = layer_->height;
    }
    canvas->save();
    canvas->concat(m);
    canvas->drawImageRect(image.get(), SkRect::MakeIWH(image->width(), image->height()),
                          SkRect::MakeIWH(w, h), SkSamplingOptions(SkFilterMode::kLinear), &paint,
                          SkCanvas::kFast_SrcRectConstraint);
    canvas->restore();
  }

 private:
  android::sp<TextureLayer> layer_;
};

TextureLayer* LayerOf(jlong ptr) { return reinterpret_cast<TextureLayer*>(ptr); }

// SkiaCanvas::drawDrawable is protected: reached through a member pointer taken in a
// derived class (never instantiated).
struct CanvasAccess : android::SkiaCanvas {
  static auto DrawDrawable() { return &CanvasAccess::drawDrawable; }
};

// Records drawable in a recording canvas (hwui's SkiaRecordingCanvas, a SkiaCanvas).
void RecordDrawable(android::Canvas* canvas, SkDrawable* drawable) {
  auto* skia = static_cast<android::SkiaCanvas*>(canvas);
  (skia->*CanvasAccess::DrawDrawable())(drawable);
}

// HardwareRenderer.nCreateTextureLayer(proxy): TextureLayer's VirtualRefBasePtr takes
// the first reference.
jlong JNICALL HR_createTextureLayer(JNIEnv*, jclass, jlong) {
  return reinterpret_cast<jlong>(new TextureLayer());
}

// A TextureView's frame copied into a Bitmap (TextureView.getBitmap).
jboolean JNICALL HR_copyLayerInto(JNIEnv*, jclass, jlong, jlong layer_ptr, jlong bitmap_ptr) {
  TextureLayer* layer = LayerOf(layer_ptr);
  sk_sp<SkImage> image = layer != nullptr ? layer->Image() : nullptr;
  if (image == nullptr) return JNI_FALSE;
  SkBitmap bitmap;
  android::bitmap::toBitmap(bitmap_ptr).getSkBitmap(&bitmap);
  SkCanvas canvas(bitmap);
  canvas.drawImageRect(image.get(), SkRect::MakeIWH(bitmap.width(), bitmap.height()),
                       SkSamplingOptions(SkFilterMode::kLinear));
  bitmap.notifyPixelsChanged();
  return JNI_TRUE;
}

// The drawable reads the latest frame when it plays back: nothing to push.
void JNICALL HR_pushLayerUpdate(JNIEnv*, jclass, jlong, jlong) {}
void JNICALL HR_cancelLayerUpdate(JNIEnv*, jclass, jlong, jlong) {}

void JNICALL HR_detachSurfaceTexture(JNIEnv*, jclass, jlong, jlong layer_ptr) {
  if (TextureLayer* layer = LayerOf(layer_ptr)) {
    std::lock_guard<std::mutex> lock(layer->mu);
    layer->surface = nullptr;
  }
}

jboolean JNICALL TL_prepare(JNIEnv*, jclass, jlong ptr, jint width, jint height, jboolean opaque) {
  TextureLayer* layer = LayerOf(ptr);
  if (layer == nullptr) return JNI_FALSE;
  std::lock_guard<std::mutex> lock(layer->mu);
  bool changed = layer->width != width || layer->height != height || layer->blend == (opaque != 0);
  layer->width = width;
  layer->height = height;
  layer->blend = !opaque;
  return changed ? JNI_TRUE : JNI_FALSE;
}

void JNICALL TL_setLayerPaint(JNIEnv*, jclass, jlong ptr, jlong paint_ptr) {
  TextureLayer* layer = LayerOf(ptr);
  if (layer == nullptr) return;
  auto* paint = reinterpret_cast<android::Paint*>(paint_ptr);
  std::lock_guard<std::mutex> lock(layer->mu);
  if (paint == nullptr) {
    layer->alpha = 255;
    layer->mode = SkBlendMode::kSrcOver;
    layer->filter = nullptr;
  } else {
    layer->alpha = paint->getAlpha();
    layer->mode = paint->asBlendMode().value_or(SkBlendMode::kSrcOver);
    layer->filter = paint->refColorFilter();
  }
}

void JNICALL TL_setTransform(JNIEnv*, jclass, jlong ptr, jlong matrix_ptr) {
  TextureLayer* layer = LayerOf(ptr);
  if (layer == nullptr) return;
  auto* m = reinterpret_cast<SkMatrix*>(matrix_ptr);
  std::lock_guard<std::mutex> lock(layer->mu);
  layer->transform = m != nullptr ? *m : SkMatrix::I();
}

void JNICALL TL_setSurfaceTexture(JNIEnv* env, jclass, jlong ptr, jobject surface_texture) {
  TextureLayer* layer = LayerOf(ptr);
  SurfaceTextureState* s = surface_texture != nullptr ? StateOf(env, surface_texture) : nullptr;
  if (layer == nullptr) return;
  std::lock_guard<std::mutex> lock(layer->mu);
  layer->surface = s != nullptr ? s->surface : nullptr;
}

void JNICALL TL_updateSurfaceTexture(JNIEnv*, jclass, jlong) {}

// RecordingCanvas.nDrawTextureLayer(canvas, layer), a @CriticalNative.
void JNICALL RC_drawTextureLayer(jlong canvas_ptr, jlong layer_ptr) {
  auto* canvas = reinterpret_cast<android::Canvas*>(canvas_ptr);
  TextureLayer* layer = LayerOf(layer_ptr);
  if (canvas == nullptr || layer == nullptr) return;
  sk_sp<SkDrawable> drawable(new TextureLayerDrawable(layer));
  RecordDrawable(canvas, drawable.get());
}

// --- SurfaceView ----------------------------------------------------------------------------

struct ViewSurface {
  android::sp<QueueSurface> surface;
  jweak view = nullptr;  // the SurfaceView, invalidated by each frame
  FrameImages images;
};

std::mutex gViewMutex;
std::map<jlong, std::shared_ptr<ViewSurface>> gViewSurfaces;  // by BLASTBufferQueue handle
// Buffer sizes BLASTBufferQueue.update gave before the surface was made.
std::map<jlong, std::pair<int, int>> gPendingSize;
jfieldID gSurfaceNativeObject;  // Surface.mNativeObject

std::shared_ptr<ViewSurface> ViewSurfaceFor(jlong blast, bool create) {
  std::lock_guard<std::mutex> lock(gViewMutex);
  auto it = gViewSurfaces.find(blast);
  if (it != gViewSurfaces.end()) return it->second;
  if (!create) return nullptr;
  auto vs = std::make_shared<ViewSurface>();
  vs->surface = android::sp<QueueSurface>::make(1, 1);
  std::weak_ptr<ViewSurface> weak = vs;
  vs->surface->SetListener([weak] {
    std::shared_ptr<ViewSurface> v = weak.lock();
    if (v == nullptr) return;
    JNIEnv* env = Env();
    if (env == nullptr) return;
    jobject view;
    {
      std::lock_guard<std::mutex> lock(gViewMutex);
      view = v->view != nullptr ? env->NewLocalRef(v->view) : nullptr;
    }
    if (view == nullptr) return;
    env->CallVoidMethod(view, gST.post_invalidate);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(view);
  });
  gViewSurfaces[blast] = vs;
  return vs;
}

class ViewSurfaceDrawable : public SkDrawable {
 public:
  ViewSurfaceDrawable(std::shared_ptr<ViewSurface> vs, int w, int h, float radius, float alpha)
      : vs_(std::move(vs)), w_(w), h_(h), radius_(radius), alpha_(alpha) {}

 protected:
  SkRect onGetBounds() override { return SkRect::MakeIWH(w_, h_); }

  void onDraw(SkCanvas* canvas) override {
    sk_sp<SkImage> image = vs_->images.Get(vs_->surface->Latest());
    if (image == nullptr) return;
    SkPaint paint;
    paint.setAlphaf(alpha_);
    canvas->save();
    if (radius_ > 0) {
      canvas->clipRRect(SkRRect::MakeRectXY(SkRect::MakeIWH(w_, h_), radius_, radius_), true);
    }
    // The buffer scaled to the view, as SurfaceFlinger scales a SurfaceView's layer.
    canvas->drawImageRect(image.get(), SkRect::MakeIWH(image->width(), image->height()),
                          SkRect::MakeIWH(w_, h_), SkSamplingOptions(SkFilterMode::kLinear), &paint,
                          SkCanvas::kFast_SrcRectConstraint);
    canvas->restore();
  }

 private:
  std::shared_ptr<ViewSurface> vs_;
  int w_, h_;
  float radius_, alpha_;
};

// SurfaceView.nativeGesherDrawSurface(canvas, blast, view, width, height, radius, alpha)
// (frameworks/base patch 0300): the surface's latest frame where the SurfaceView is.
void JNICALL SV_drawSurface(JNIEnv* env, jclass, jlong canvas_ptr, jlong blast, jobject view,
                            jint width, jint height, jfloat radius, jfloat alpha) {
  auto* canvas = reinterpret_cast<android::Canvas*>(canvas_ptr);
  std::shared_ptr<ViewSurface> vs = ViewSurfaceFor(blast, false);
  if (canvas == nullptr || vs == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(gViewMutex);
    if (vs->view == nullptr || !env->IsSameObject(vs->view, view)) {
      if (vs->view != nullptr) env->DeleteWeakGlobalRef(vs->view);
      vs->view = env->NewWeakGlobalRef(view);
    }
  }
  sk_sp<SkDrawable> drawable(new ViewSurfaceDrawable(vs, width, height, radius, alpha));
  RecordDrawable(canvas, drawable.get());
}

// BLASTBufferQueue.nativeUpdate: the stand-in's tagging (a window's adapter), and a
// SurfaceView's buffer size.
void JNICALL BBQ_update(JNIEnv* env, jobject thiz, jlong ptr, jlong surface_control, jlong width,
                        jlong height, jint format) {
  gesher_device_Blast_update(env, thiz, ptr, surface_control, width, height, format);
  if (std::shared_ptr<ViewSurface> vs = ViewSurfaceFor(ptr, false)) {
    vs->surface->SetDefaultSize(static_cast<int>(width), static_cast<int>(height));
  }
  {
    std::lock_guard<std::mutex> lock(gViewMutex);
    auto it = gViewSurfaces.find(ptr);
    if (it == gViewSurfaces.end()) {
      // Remember the size for the surface made later (nativeGetFromBlastBufferQueue).
      gPendingSize[ptr] = {static_cast<int>(width), static_cast<int>(height)};
    }
  }
}

void JNICALL BBQ_destroy(JNIEnv* env, jclass, jlong ptr) {
  std::shared_ptr<ViewSurface> vs;
  {
    std::lock_guard<std::mutex> lock(gViewMutex);
    auto it = gViewSurfaces.find(ptr);
    if (it != gViewSurfaces.end()) {
      vs = it->second;
      gViewSurfaces.erase(it);
    }
    gPendingSize.erase(ptr);
    if (vs != nullptr && vs->view != nullptr) {
      env->DeleteWeakGlobalRef(vs->view);
      vs->view = nullptr;
    }
  }
}

android::sp<QueueSurface> SurfaceForBlast(jlong blast) {
  std::shared_ptr<ViewSurface> vs = ViewSurfaceFor(blast, true);
  std::lock_guard<std::mutex> lock(gViewMutex);
  auto it = gPendingSize.find(blast);
  if (it != gPendingSize.end()) {
    vs->surface->SetDefaultSize(it->second.first, it->second.second);
    gPendingSize.erase(it);
  }
  return vs->surface;
}

// BLASTBufferQueue.nativeGetSurface: a window's adapter gives the window's Surface (the
// stand-in's provider); any other (a SurfaceView's) its QueueSurface.
jobject JNICALL BBQ_getSurface(JNIEnv* env, jclass clazz, jlong ptr, jboolean handle) {
  jobject s = gesher_device_Blast_getSurface(env, clazz, ptr, handle);
  if (s != nullptr) return s;
  android::sp<android::Surface> surface = SurfaceForBlast(ptr);
  return android::android_view_Surface_createFromSurface(env, surface);
}

// Surface.nativeGetFromBlastBufferQueue(nativeObject, blast).
jlong JNICALL Surface_getFromBlastBufferQueue(JNIEnv* env, jclass, jlong native_object, jlong blast) {
  // A window's adapter: the window's Surface (from the stand-ins' provider).
  jobject window_surface = gesher_device_Blast_getSurface(env, nullptr, blast, JNI_FALSE);
  if (window_surface != nullptr) {
    jlong p = env->GetLongField(window_surface, gSurfaceNativeObject);
    env->DeleteLocalRef(window_surface);
    if (p == native_object || p == 0) return p;
    reinterpret_cast<android::Surface*>(p)->incStrong(reinterpret_cast<void*>(&Surface_getFromBlastBufferQueue));
    return p;
  }
  android::sp<QueueSurface> surface = SurfaceForBlast(blast);
  auto* raw = static_cast<android::Surface*>(surface.get());
  if (reinterpret_cast<jlong>(raw) == native_object) return native_object;
  raw->incStrong(reinterpret_cast<void*>(&Surface_getFromBlastBufferQueue));
  return reinterpret_cast<jlong>(raw);
}

void Register(JNIEnv* env, const char* class_name, const JNINativeMethod* methods, int count) {
  jclass c = env->FindClass(class_name);
  if (c == nullptr) {
    env->ExceptionClear();
    ALOGI("gesher-media: %s not on the class path; its surface natives skipped", class_name);
    return;
  }
  for (int i = 0; i < count; i++) {
    if (env->RegisterNatives(c, &methods[i], 1) != 0) {
      env->ExceptionClear();
      ALOGW("gesher-media: %s.%s%s not registered", class_name, methods[i].name, methods[i].signature);
    }
  }
  env->DeleteLocalRef(c);
}

#define N(fn) reinterpret_cast<void*>(&(fn))

const JNINativeMethod kSurfaceTexture[] = {
    {"nativeInit", "(ZIZLjava/lang/ref/WeakReference;)V", N(ST_init)},
    {"nativeFinalize", "()V", N(ST_finalize)},
    {"nativeGetTransformMatrix", "([F)V", N(ST_getTransformMatrix)},
    {"nativeGetTimestamp", "()J", N(ST_getTimestamp)},
    {"nativeGetDataSpace", "()I", N(ST_getDataSpace)},
    {"nativeSetDefaultBufferSize", "(II)V", N(ST_setDefaultBufferSize)},
    {"nativeUpdateTexImage", "()V", N(ST_updateTexImage)},
    {"nativeReleaseTexImage", "()V", N(ST_releaseTexImage)},
    {"nativeDetachFromGLContext", "()I", N(ST_detachFromGLContext)},
    {"nativeAttachToGLContext", "(I)I", N(ST_attachToGLContext)},
    {"nativeRelease", "()V", N(ST_release)},
    {"nativeIsReleased", "()Z", N(ST_isReleased)},
};

const JNINativeMethod kSurface[] = {
    {"nativeCreateFromSurfaceTexture", "(Landroid/graphics/SurfaceTexture;)J",
     N(Surface_createFromSurfaceTexture)},
    {"nativeGetFromBlastBufferQueue", "(JJ)J", N(Surface_getFromBlastBufferQueue)},
};

const JNINativeMethod kHardwareRenderer[] = {
    {"nCreateTextureLayer", "(J)J", N(HR_createTextureLayer)},
    {"nCopyLayerInto", "(JJJ)Z", N(HR_copyLayerInto)},
    {"nPushLayerUpdate", "(JJ)V", N(HR_pushLayerUpdate)},
    {"nCancelLayerUpdate", "(JJ)V", N(HR_cancelLayerUpdate)},
    {"nDetachSurfaceTexture", "(JJ)V", N(HR_detachSurfaceTexture)},
};

const JNINativeMethod kTextureLayer[] = {
    {"nPrepare", "(JIIZ)Z", N(TL_prepare)},
    {"nSetLayerPaint", "(JJ)V", N(TL_setLayerPaint)},
    {"nSetTransform", "(JJ)V", N(TL_setTransform)},
    {"nSetSurfaceTexture", "(JLandroid/graphics/SurfaceTexture;)V", N(TL_setSurfaceTexture)},
    {"nUpdateSurfaceTexture", "(J)V", N(TL_updateSurfaceTexture)},
};

const JNINativeMethod kRecordingCanvas[] = {
    {"nDrawTextureLayer", "(JJ)V", N(RC_drawTextureLayer)},
};

const JNINativeMethod kBlast[] = {
    {"nativeUpdate", "(JJJJI)V", N(BBQ_update)},
    {"nativeGetSurface", "(JZ)Landroid/view/Surface;", N(BBQ_getSurface)},
    {"nativeDestroy", "(J)V", N(BBQ_destroy)},
};

const JNINativeMethod kTextureView[] = {
    {"nCreateNativeWindow", "(Landroid/graphics/SurfaceTexture;)V", N(TV_createNativeWindow)},
    {"nDestroyNativeWindow", "()V", N(TV_destroyNativeWindow)},
    {"nLockCanvas", "(JLandroid/graphics/Canvas;Landroid/graphics/Rect;)Z", N(TV_lockCanvas)},
    {"nUnlockCanvasAndPost", "(JLandroid/graphics/Canvas;)V", N(TV_unlockCanvasAndPost)},
};

const JNINativeMethod kSurfaceView[] = {
    {"nativeGesherDrawSurface", "(JJLandroid/view/SurfaceView;IIFF)V", N(SV_drawSurface)},
};

}  // namespace

// Called by register_gesher_media (compat/media/jni/audio_system_jni.cc).
int register_gesher_media_surfaces(JNIEnv* env) {
  env->GetJavaVM(&gVm);
  jclass st = env->FindClass("android/graphics/SurfaceTexture");
  if (st != nullptr) {
    gST.surface_texture_class = static_cast<jclass>(env->NewGlobalRef(st));
    gST.surface_texture = env->GetFieldID(st, "mSurfaceTexture", "J");
    gST.post_event = env->GetStaticMethodID(st, "postEventFromNative", "(Ljava/lang/ref/WeakReference;)V");
    env->DeleteLocalRef(st);
  }
  jclass surface_class = env->FindClass("android/view/Surface");
  if (surface_class != nullptr) {
    gSurfaceNativeObject = env->GetFieldID(surface_class, "mNativeObject", "J");
    env->DeleteLocalRef(surface_class);
  }
  jclass view = env->FindClass("android/view/View");
  if (view != nullptr) {
    gST.post_invalidate = env->GetMethodID(view, "postInvalidate", "()V");
    env->DeleteLocalRef(view);
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
#define REG(cls, table) Register(env, cls, table, sizeof(table) / sizeof(table[0]))
  REG("android/graphics/SurfaceTexture", kSurfaceTexture);
  REG("android/view/Surface", kSurface);
  REG("android/graphics/HardwareRenderer", kHardwareRenderer);
  REG("android/graphics/TextureLayer", kTextureLayer);
  REG("android/graphics/RecordingCanvas", kRecordingCanvas);
  REG("android/graphics/BLASTBufferQueue", kBlast);
  REG("android/view/SurfaceView", kSurfaceView);
  REG("android/view/TextureView", kTextureView);
#undef REG
  return 0;
}
