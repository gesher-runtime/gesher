// Test the real pipeline_select.cc without AOSP or hardware.
#include "../../compat/hwui/gl/pipeline_select.h"
#include <cstdio>
#include <cstdlib>
#include <string>
static int g_probes = 0;
static bool g_probe_ok = true;
static const char* g_renderer = nullptr;
bool gesher_hwui_gl_usable(std::string* detail, bool) {
  ++g_probes;
  *detail = g_renderer ? g_renderer : "ANGLE device unavailable";
  return g_probe_ok;
}
void Env(const char* name, const char* value) {
#ifdef _WIN32
  if (_putenv_s(name, value ? value : "") != 0) std::abort();
#else
  if (value ? setenv(name, value, 1) != 0 : unsetenv(name) != 0) std::abort();
#endif
}
void Check(const char* label, const char* window, const char* renderer,
           const char* override_renderer, const char* egl_device, bool probe_ok,
           android::uirenderer::RenderPipelineType expected, int expected_probes) {
  Env("GESHER_APP_WINDOW", window);
  Env("GESHER_HWUI_RENDERER", override_renderer);
  Env("GESHER_TEST_RENDERER_PROPERTY", nullptr);
  Env("GESHER_EGL_DEVICE", egl_device);
  g_renderer = renderer;
  g_probe_ok = probe_ok;
  g_probes = 0;
  auto selected = gesher_hwui_choose_pipeline();
  if (selected != expected || g_probes != expected_probes) {
    std::fprintf(stderr, "FAIL %s: result=%d probes=%d\n", label,
                 static_cast<int>(selected), g_probes);
    std::exit(1);
  }
}
int main() {
  using android::uirenderer::RenderPipelineType;
  const char* intel = "ANGLE (Intel, Intel(R) Iris Xe Graphics Direct3D11 vs_5_0 ps_5_0)";
  const char* warp = "ANGLE (Microsoft, Microsoft Basic Render Driver Direct3D11 vs_5_0)";
  Check("headless no probe", "headless", intel, nullptr, nullptr, true, RenderPipelineType::SkiaCpu, 0);
  Check("launcher no probe", nullptr, intel, nullptr, nullptr, true, RenderPipelineType::SkiaCpu, 0);
  Check("Intel window default avoids probe", "window", intel, nullptr, nullptr, true, RenderPipelineType::SkiaCpu, 0);
  Check("Intel hidden default avoids probe", "hidden", intel, nullptr, nullptr, true, RenderPipelineType::SkiaCpu, 0);
  Check("explicit auto Intel window", "window", intel, "auto", nullptr, true, RenderPipelineType::SkiaGL, 1);
  Check("software fallback", "window", warp, "auto", nullptr, true, RenderPipelineType::SkiaCpu, 1);
  Check("unavailable fallback", "window", nullptr, "auto", nullptr, false, RenderPipelineType::SkiaCpu, 1);
  Check("force CPU", "window", intel, "skiacpu", nullptr, true, RenderPipelineType::SkiaCpu, 0);
  Check("explicit GL on WARP", "window", warp, "skiagl", "warp", true, RenderPipelineType::SkiaGL, 1);
  Check("auto avoids WARP", "window", intel, "auto", "warp", true, RenderPipelineType::SkiaCpu, 0);
  Check("explicit auto", "headless", intel, "auto", nullptr, true, RenderPipelineType::SkiaGL, 1);
  Check("invalid mode", "window", intel, "skiavk", nullptr, true, RenderPipelineType::SkiaCpu, 0);
  Env("GESHER_APP_WINDOW", "headless");
  Env("GESHER_HWUI_RENDERER", nullptr);
  Env("GESHER_EGL_DEVICE", "warp");
  Env("GESHER_TEST_RENDERER_PROPERTY", "skiagl");
  g_renderer = warp; g_probe_ok = true; g_probes = 0;
  if (gesher_hwui_choose_pipeline() != RenderPipelineType::SkiaGL || g_probes != 1) {
    std::fputs("FAIL explicit property override\n", stderr);
    return 1;
  }
  std::puts("hwui GPU pipeline selection all passed");
}
