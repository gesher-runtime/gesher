// Gesher: choosing hwui's pipeline (docs/angle.md, ADR 0030).
//
// The CPU pipeline (SkiaCpu) stays the default: it needs nothing but the
// window's back buffer. SkiaGL (Skia's Ganesh over ANGLE's OpenGL ES on
// Direct3D 11) is chosen with GESHER_HWUI_RENDERER=skiagl in the environment
// or the system property debug.hwui.renderer=skiagl (Android's own switch;
// $ANDROID_ROOT/build.prop or android.os.SystemProperties before
// libandroid_runtime loads). The environment variable wins. hwui fixes the
// pipeline for the process (Properties::overrideRenderPipelineType) before
// anything draws, and EglManager aborts when EGL fails, so EGL is tried here
// first (gesher_hwui_gl_usable): if it does not work, a line says why and the
// CPU pipeline renders. "skiavk" is not built and also gives the CPU pipeline.

#include "pipeline_select.h"

#include <android-base/properties.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "angle_gl.h"

namespace {

using android::uirenderer::RenderPipelineType;

const char* g_name = "skiacpu";
std::string* g_detail = new std::string("the default");

void Say(const std::string& text) {
  std::fprintf(stderr, "gesher hwui: %s\n", text.c_str());
  std::fflush(stderr);
}

}  // namespace

RenderPipelineType gesher_hwui_choose_pipeline() {
  std::string want;
  std::string from;
  if (const char* env = std::getenv("GESHER_HWUI_RENDERER"); env != nullptr && *env != '\0') {
    want = env;
    from = "GESHER_HWUI_RENDERER";
  } else {
    want = android::base::GetProperty("debug.hwui.renderer", "");
    from = "debug.hwui.renderer";
  }
  if (want.empty() || want == "skiacpu") return RenderPipelineType::SkiaCpu;
  if (want != "skiagl") {
    *g_detail = from + "=" + want + ": not built";
    Say(*g_detail + "; using the CPU pipeline (SkiaCpu)");
    return RenderPipelineType::SkiaCpu;
  }
  std::string detail;
  if (!gesher_hwui_gl_usable(&detail)) {
    *g_detail = from + "=skiagl: " + detail;
    Say(from + "=skiagl, but OpenGL ES over ANGLE is not usable (" + detail +
        "); using the CPU pipeline (SkiaCpu)");
    return RenderPipelineType::SkiaCpu;
  }
  g_name = "skiagl";
  *g_detail = from + "=skiagl: " + detail;
  Say("SkiaGL pipeline (" + from + "=skiagl): ANGLE " + detail);
  return RenderPipelineType::SkiaGL;
}

const char* gesher_hwui_renderer_name() {
  return g_name;
}

const char* gesher_hwui_renderer_detail() {
  return g_detail->c_str();
}
