// Gesher: hwui's JNI registrations, run from libandroid_runtime's table
// (compat/framework/android_runtime.cc). The list and its order are those of
// hwui's apex/jni_runtime.cpp on a device (ColorSpace before Graphics, which
// looks up ColorSpace.get()), less the device-only classes (TextureLayer,
// Movie, PdfDocument, PdfEditor) and YuvImage (libultrahdr), plus Surface,
// whose natives libandroid_runtime has, and gesher-app's Win32 window. hwui renders with its CPU pipeline
// (SkiaCpu) until the GL one runs over ANGLE (docs/graphics.md).
//
// Each registration is skipped, with a log line, when its class is not on the
// class path, as android_runtime.cc does for its own.

#include <jni.h>

#include <log/log.h>

#include "FrameMetricsObserver.h"
#include "GraphicsJNI.h"
#include "Properties.h"
#include "include/core/SkGraphics.h"

extern int register_android_graphics_Bitmap(JNIEnv*);
extern int register_android_graphics_BitmapFactory(JNIEnv*);
extern int register_android_graphics_BitmapRegionDecoder(JNIEnv*);
extern int register_android_graphics_RuntimeXfermode(JNIEnv*);
extern int register_android_graphics_ByteBufferStreamAdaptor(JNIEnv* env);
extern int register_android_graphics_Camera(JNIEnv* env);
extern int register_android_graphics_CreateJavaOutputStreamAdaptor(JNIEnv* env);
extern int register_android_graphics_Graphics(JNIEnv* env);
extern int register_android_graphics_ImageDecoder(JNIEnv*);
extern int register_android_graphics_drawable_AnimatedImageDrawable(JNIEnv*);
extern int register_android_graphics_Interpolator(JNIEnv* env);
extern int register_android_graphics_MaskFilter(JNIEnv* env);
extern int register_android_graphics_NinePatch(JNIEnv*);
extern int register_android_graphics_PathEffect(JNIEnv* env);
extern int register_android_graphics_Shader(JNIEnv* env);
extern int register_android_graphics_RenderEffect(JNIEnv* env);
extern int register_android_graphics_Typeface(JNIEnv* env);

namespace android {
extern int register_android_graphics_Canvas(JNIEnv* env);
extern int register_android_graphics_CanvasProperty(JNIEnv* env);
extern int register_android_graphics_ColorFilter(JNIEnv* env);
extern int register_android_graphics_Color(JNIEnv* env);
extern int register_android_graphics_ColorSpace(JNIEnv* env);
extern int register_android_graphics_DrawFilter(JNIEnv* env);
extern int register_android_graphics_FontFamily(JNIEnv* env);
extern int register_android_graphics_Gainmap(JNIEnv* env);
extern int register_android_graphics_HardwareRendererObserver(JNIEnv* env);
extern int register_android_graphics_Matrix(JNIEnv* env);
extern int register_android_graphics_Paint(JNIEnv* env);
extern int register_android_graphics_Path(JNIEnv* env);
extern int register_android_graphics_PathIterator(JNIEnv* env);
extern int register_android_graphics_PathMeasure(JNIEnv* env);
extern int register_android_graphics_Picture(JNIEnv*);
extern int register_android_graphics_Region(JNIEnv* env);
extern int register_android_graphics_animation_NativeInterpolatorFactory(JNIEnv* env);
extern int register_android_graphics_animation_RenderNodeAnimator(JNIEnv* env);
extern int register_android_graphics_drawable_AnimatedVectorDrawable(JNIEnv* env);
extern int register_android_graphics_drawable_VectorDrawable(JNIEnv* env);
extern int register_android_graphics_fonts_Font(JNIEnv* env);
extern int register_android_graphics_fonts_FontFamily(JNIEnv* env);
extern int register_android_graphics_text_MeasuredText(JNIEnv* env);
extern int register_android_graphics_text_LineBreaker(JNIEnv* env);
extern int register_android_graphics_text_TextShaper(JNIEnv* env);
extern int register_android_graphics_text_GraphemeBreak(JNIEnv* env);
extern int register_android_graphics_MeshSpecification(JNIEnv* env);
extern int register_android_graphics_Mesh(JNIEnv* env);
extern int register_android_util_PathParser(JNIEnv* env);
extern int register_android_view_DisplayListCanvas(JNIEnv* env);
extern int register_android_view_RenderNode(JNIEnv* env);
extern int register_android_view_ThreadedRenderer(JNIEnv* env);
extern int register_android_graphics_HardwareBufferRenderer(JNIEnv* env);
extern int register_android_view_Surface(JNIEnv* env);
}  // namespace android

// compat/hwui/window/window_jni.cc: gesher-app's window (Win32Surface).
extern int register_com_gesher_app_window_NativeWindow(JNIEnv* env);

namespace {

using namespace android;

struct Registration {
  const char* className;  // a class the registration needs
  int (*fn)(JNIEnv*);
};

const Registration kRegistrations[] = {
    {"android/graphics/Canvas", register_android_graphics_Canvas},
    {"android/graphics/Color", register_android_graphics_Color},
    {"android/graphics/ColorSpace", register_android_graphics_ColorSpace},
    {"android/graphics/Bitmap", register_android_graphics_Graphics},
    {"android/graphics/Bitmap", register_android_graphics_Bitmap},
    {"android/graphics/BitmapFactory", register_android_graphics_BitmapFactory},
    {"android/graphics/BitmapRegionDecoder", register_android_graphics_BitmapRegionDecoder},
    {"android/graphics/RuntimeXfermode", register_android_graphics_RuntimeXfermode},
    {"android/graphics/ImageDecoder", register_android_graphics_ByteBufferStreamAdaptor},
    {"android/graphics/Camera", register_android_graphics_Camera},
    {"android/graphics/BitmapFactory", register_android_graphics_CreateJavaOutputStreamAdaptor},
    {"android/graphics/CanvasProperty", register_android_graphics_CanvasProperty},
    {"android/graphics/ColorFilter", register_android_graphics_ColorFilter},
    {"android/graphics/DrawFilter", register_android_graphics_DrawFilter},
    {"android/graphics/FontFamily", register_android_graphics_FontFamily},
    {"android/graphics/Gainmap", register_android_graphics_Gainmap},
    {"android/graphics/HardwareRendererObserver",
     register_android_graphics_HardwareRendererObserver},
    {"android/graphics/ImageDecoder", register_android_graphics_ImageDecoder},
    {"android/graphics/drawable/AnimatedImageDrawable",
     register_android_graphics_drawable_AnimatedImageDrawable},
    {"android/graphics/Interpolator", register_android_graphics_Interpolator},
    {"android/graphics/MaskFilter", register_android_graphics_MaskFilter},
    {"android/graphics/Matrix", register_android_graphics_Matrix},
    {"android/graphics/NinePatch", register_android_graphics_NinePatch},
    {"android/graphics/Paint", register_android_graphics_Paint},
    {"android/graphics/Path", register_android_graphics_Path},
    {"android/graphics/PathIterator", register_android_graphics_PathIterator},
    {"android/graphics/PathMeasure", register_android_graphics_PathMeasure},
    {"android/graphics/PathEffect", register_android_graphics_PathEffect},
    {"android/graphics/Picture", register_android_graphics_Picture},
    {"android/graphics/Region", register_android_graphics_Region},
    {"android/graphics/Shader", register_android_graphics_Shader},
    {"android/graphics/RenderEffect", register_android_graphics_RenderEffect},
    {"android/graphics/Typeface", register_android_graphics_Typeface},
    {"android/graphics/animation/NativeInterpolatorFactory",
     register_android_graphics_animation_NativeInterpolatorFactory},
    {"android/graphics/animation/RenderNodeAnimator",
     register_android_graphics_animation_RenderNodeAnimator},
    {"android/graphics/drawable/AnimatedVectorDrawable",
     register_android_graphics_drawable_AnimatedVectorDrawable},
    {"android/graphics/drawable/VectorDrawable", register_android_graphics_drawable_VectorDrawable},
    {"android/graphics/fonts/Font", register_android_graphics_fonts_Font},
    {"android/graphics/fonts/FontFamily", register_android_graphics_fonts_FontFamily},
    {"android/graphics/text/MeasuredText", register_android_graphics_text_MeasuredText},
    {"android/graphics/text/LineBreaker", register_android_graphics_text_LineBreaker},
    {"android/graphics/text/TextRunShaper", register_android_graphics_text_TextShaper},
    {"android/graphics/text/GraphemeBreak", register_android_graphics_text_GraphemeBreak},
    {"android/graphics/MeshSpecification", register_android_graphics_MeshSpecification},
    {"android/graphics/Mesh", register_android_graphics_Mesh},
    {"android/util/PathParser", register_android_util_PathParser},
    {"android/graphics/RenderNode", register_android_view_RenderNode},
    {"android/graphics/RecordingCanvas", register_android_view_DisplayListCanvas},
    {"android/graphics/HardwareBufferRenderer", register_android_graphics_HardwareBufferRenderer},
    {"android/graphics/HardwareRenderer", register_android_view_ThreadedRenderer},
    {"android/view/Surface", register_android_view_Surface},
    {"com/gesher/app/window/NativeWindow", register_com_gesher_app_window_NativeWindow},
};

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

int register_gesher_graphics(JNIEnv* env) {
  JavaVM* vm = nullptr;
  env->GetJavaVM(&vm);
  GraphicsJNI::setJavaVM(vm);
  android::uirenderer::Properties::overrideRenderPipelineType(
      android::uirenderer::RenderPipelineType::SkiaCpu);
  SkGraphics::Init();
  for (const Registration& r : kRegistrations) {
    if (!HasClass(env, r.className)) {
      ALOGI("graphics: %s is not on the class path; its natives are not registered",
            r.className);
      continue;
    }
    if (r.fn(env) < 0) {
      ALOGE("graphics: registering the natives that need %s failed", r.className);
      return -1;
    }
  }
  return 0;
}

namespace {

// Frame metrics nobody reads.
class DroppingFrameMetricsObserver : public android::uirenderer::FrameMetricsObserver {
 public:
  DroppingFrameMetricsObserver() : FrameMetricsObserver(/*waitForPresentTime=*/false) {}
  void notify(const android::uirenderer::FrameInfoBuffer&) override {}
};

}  // namespace

// The stand-in for InputEventReceiver.nativeGetFrameMetricsObserver
// (compat/framework/attach_standins.cc), while the app process's input is a
// stand-in: ViewRootImpl hands the answer to HardwareRenderer.nAddObserver,
// whose render thread calls it, so it must be an observer (0 crashes
// CanvasContext::addFrameMetricsObserver). One for the process, with a strong
// reference that is never dropped: nAddObserver takes another with
// sp::fromExisting, which needs one, and nRemoveObserver later passes the
// same pointer.
extern "C" jlong gesher_standin_frame_metrics_observer() {
  static android::uirenderer::FrameMetricsObserver* const observer = [] {
    auto* o = new DroppingFrameMetricsObserver();
    o->incStrong(nullptr);
    return o;
  }();
  return reinterpret_cast<jlong>(observer);
}
