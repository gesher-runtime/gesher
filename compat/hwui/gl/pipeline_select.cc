// Gesher: choosing hwui's pipeline (docs/angle.md, ADR 0030).
//
// CPU remains the default until real Windows hardware acceptance is complete.
// Explicit auto qualifies real ANGLE D3D11 hardware, including Intel graphics.
// GESHER_HWUI_RENDERER and debug.hwui.renderer explicitly override with
// "auto", "skiacpu" or "skiagl"; explicit skiagl permits WARP for tests.
// If EGL/Skia cannot initialize, use the original CPU pipeline.

#include "pipeline_select.h"

#include <android-base/properties.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "angle_gl.h"
#include "hardware_renderer_policy.h"

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
  // A second explicit choice must not retain stale diagnostics from a prior
  // invocation (registration normally selects once per process).
  g_name = "skiacpu";
  *g_detail = "the default";
  std::string want;
  std::string from;
  if (const char* env = std::getenv("GESHER_HWUI_RENDERER"); env != nullptr && *env != '\0') {
    want = env;
    from = "GESHER_HWUI_RENDERER";
  } else {
    want = android::base::GetProperty("debug.hwui.renderer", "");
    from = "debug.hwui.renderer";
  }
  // Window presence alone does not qualify the GL implementation's complete
  // app workflow, memory retirement or device-loss behavior on real hardware.
  if (want.empty() || want == "skiacpu") return RenderPipelineType::SkiaCpu;
  const bool automatic = want == "auto";
  if (!automatic && want != "skiagl") {
    *g_detail = from + "=" + want + ": not built";
    Say(*g_detail + "; using the CPU pipeline (SkiaCpu)");
    return RenderPipelineType::SkiaCpu;
  }
  if (automatic) {
    // An explicit request for D3D11's software WARP device is not evidence
    // of available hardware acceleration.
    const char* device = std::getenv("GESHER_EGL_DEVICE");
    if (device != nullptr && std::string_view(device) == "warp") {
      *g_detail = from + "=auto: GESHER_EGL_DEVICE=warp; using SkiaCpu";
      return RenderPipelineType::SkiaCpu;
    }
  }
  std::string detail;
  if (!gesher_hwui_gl_usable(&detail, automatic)) {
    *g_detail = from + "=" + want + ": " + detail;
    Say(from + "=" + want + ", but OpenGL ES over ANGLE is not usable (" + detail +
        "); using the CPU pipeline (SkiaCpu)");
    return RenderPipelineType::SkiaCpu;
  }
  if (automatic && !gesher::hwui::HardwareAngleD3D11(detail)) {
    *g_detail = from + "=auto: software or unrecognized renderer (" + detail +
                "); using SkiaCpu";
    Say(*g_detail);
    return RenderPipelineType::SkiaCpu;
  }
  g_name = "skiagl";
  *g_detail = from + "=" + want + ": " + detail;
  Say("SkiaGL pipeline (" + from + "=" + want + "): ANGLE " + detail);
  return RenderPipelineType::SkiaGL;
}

const char* gesher_hwui_renderer_name() {
  return g_name;
}

const char* gesher_hwui_renderer_detail() {
  return g_detail->c_str();
}
