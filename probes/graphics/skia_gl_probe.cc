// Skia's Ganesh GL backend over ANGLE (OpenGL ES on Direct3D 11), as hwui's
// SkiaGL pipeline uses it (docs/angle.md, ADR 0030): an EGL display on
// ANGLE's D3D11 device (the hardware adapter, else WARP; --warp forces WARP),
// an ES context on a pbuffer, GrGLMakeNativeInterface
// (compat/hwui/gl/GrGLMakeNativeInterface_angle.cc) and a GrDirectContext;
// then shapes, text and a bitmap drawn on the GPU are read back and compared
// with the same drawing by Skia's CPU raster backend. libEGL.dll and
// libGLESv2.dll are found next to the probe. Prints "all passed" when every
// check holds. Run in CI on Windows (tests/windows/tracks/31-angle.ps1).
//
//   skia_gl_probe [--warp] [font.ttf]
#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/ports/SkFontMgr_empty.h"

#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>

#ifndef EGL_PLATFORM_ANGLE_ANGLE
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE 0x3208
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE 0x3209
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE 0x320A
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE 0x320B
#endif

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

HMODULE LoadNextToExe(const wchar_t* name) {
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring p(path, n);
  size_t slash = p.find_last_of(L"\\/");
  if (slash != std::wstring::npos) p.resize(slash + 1);
  p += name;
  return LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

HMODULE g_egl = nullptr;

template <typename F>
F Egl(const char* name) {
  auto p = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(g_egl, name)));
  if (p == nullptr) {
    std::printf("FAIL  libEGL.dll has no %s\n", name);
    std::exit(1);
  }
  return p;
}
#define EGL(name) Egl<decltype(&::name)>(#name)

// What both backends draw: white, a magenta rectangle, an antialiased blue
// circle, a 16x16 bitmap of four colored quadrants scaled 2x (nearest), and
// "Gesher" in the given font.
SkBitmap Quadrants() {
  SkBitmap bitmap;
  bitmap.allocN32Pixels(16, 16);
  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 16; x++) {
      SkColor c = y < 8 ? (x < 8 ? SK_ColorRED : SK_ColorGREEN)
                        : (x < 8 ? SK_ColorBLUE : SK_ColorYELLOW);
      *bitmap.getAddr32(x, y) = SkPreMultiplyColor(c);
    }
  }
  bitmap.setImmutable();
  return bitmap;
}

void Draw(SkCanvas* canvas, const sk_sp<SkTypeface>& typeface, const sk_sp<SkImage>& image) {
  canvas->clear(SK_ColorWHITE);
  SkPaint magenta;
  magenta.setColor(SK_ColorMAGENTA);
  canvas->drawRect(SkRect::MakeLTRB(8, 8, 56, 56), magenta);
  SkPaint blue;
  blue.setColor(SK_ColorBLUE);
  blue.setAntiAlias(true);
  canvas->drawCircle(96, 32, 22, blue);
  canvas->drawImageRect(image, SkRect::MakeXYWH(136, 8, 32, 32),
                        SkSamplingOptions(SkFilterMode::kNearest), nullptr);
  if (typeface) {
    SkFont font(typeface, 26);
    SkPaint black;
    black.setColor(SK_ColorBLACK);
    black.setAntiAlias(true);
    canvas->drawString("Gesher", 8, 88, font, black);
  }
}

int Diff(SkColor a, SkColor b) {
  int d = std::abs(int(SkColorGetR(a)) - int(SkColorGetR(b)));
  d = std::max(d, std::abs(int(SkColorGetG(a)) - int(SkColorGetG(b))));
  return std::max(d, std::abs(int(SkColorGetB(a)) - int(SkColorGetB(b))));
}

}  // namespace

int main(int argc, char** argv) {
  bool warp = false;
  const char* font_path = "C:/Windows/Fonts/arial.ttf";
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--warp") == 0) {
      warp = true;
    } else {
      font_path = argv[i];
    }
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  LoadNextToExe(L"libGLESv2.dll");
  g_egl = LoadNextToExe(L"libEGL.dll");
  check(g_egl != nullptr, "ANGLE's libEGL.dll and libGLESv2.dll load");
  if (g_egl == nullptr) return 1;

  auto get_display = Egl<PFNEGLGETPLATFORMDISPLAYEXTPROC>("eglGetPlatformDisplayEXT");
  EGLDisplay display = EGL_NO_DISPLAY;
  const char* device = "WARP";
  for (EGLint type : {EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE,
                      EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE}) {
    if (warp && type == EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE) continue;
    const EGLint attribs[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
                              EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, type, EGL_NONE};
    EGLDisplay d = get_display(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attribs);
    EGLint major = 0, minor = 0;
    if (d != EGL_NO_DISPLAY && EGL(eglInitialize)(d, &major, &minor)) {
      display = d;
      device = type == EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE ? "hardware" : "WARP";
      break;
    }
  }
  check(display != EGL_NO_DISPLAY, "EGL display on ANGLE's Direct3D 11 device");
  if (display == EGL_NO_DISPLAY) return 1;

  const EGLint config_attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
                                   EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                   EGL_STENCIL_SIZE, 8, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                   EGL_NONE};
  EGLConfig config = nullptr;
  EGLint configs = 0;
  bool ok = EGL(eglChooseConfig)(display, config_attribs, &config, 1, &configs) && configs == 1;
  const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext context = ok ? EGL(eglCreateContext)(display, config, EGL_NO_CONTEXT, context_attribs)
                          : EGL_NO_CONTEXT;
  const int w = 192, h = 96;
  const EGLint pbuffer_attribs[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
  EGLSurface pbuffer = context != EGL_NO_CONTEXT
                           ? EGL(eglCreatePbufferSurface)(display, config, pbuffer_attribs)
                           : EGL_NO_SURFACE;
  ok = pbuffer != EGL_NO_SURFACE && EGL(eglMakeCurrent)(display, pbuffer, pbuffer, context);
  check(ok, "RGBA8 config, an ES context on a pbuffer, current");
  if (!ok) return 1;

  sk_sp<const GrGLInterface> gl = GrGLMakeNativeInterface();
  check(gl != nullptr && gl->validate(), "GrGLMakeNativeInterface: ANGLE's OpenGL ES functions");
  if (gl == nullptr) return 1;
  using GetString = const unsigned char*(__stdcall*)(unsigned);
  auto get_string = reinterpret_cast<GetString>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"libGLESv2.dll"), "glGetString")));
  std::printf("INFO  device %s; GL_RENDERER %s; GL_VERSION %s\n", device,
              get_string ? reinterpret_cast<const char*>(get_string(0x1F01)) : "?",
              get_string ? reinterpret_cast<const char*>(get_string(0x1F02)) : "?");
  sk_sp<GrDirectContext> gr = GrDirectContexts::MakeGL(gl);
  check(gr != nullptr, "GrDirectContexts::MakeGL over ANGLE");
  if (gr == nullptr) return 1;

  sk_sp<SkSurface> gpu = SkSurfaces::RenderTarget(gr.get(), skgpu::Budgeted::kNo,
                                                  SkImageInfo::MakeN32Premul(w, h));
  check(gpu != nullptr, "Ganesh render target surface");
  if (gpu == nullptr) return 1;
  sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_Custom_Empty();
  sk_sp<SkTypeface> typeface = font_mgr ? font_mgr->makeFromFile(font_path) : nullptr;
  check(typeface != nullptr, "FreeType loads the font");
  SkBitmap quadrants = Quadrants();
  sk_sp<SkImage> image = quadrants.asImage();
  Draw(gpu->getCanvas(), typeface, image);
  skgpu::ganesh::FlushAndSubmit(gpu);

  SkBitmap from_gpu;
  from_gpu.allocN32Pixels(w, h);
  check(gpu->readPixels(from_gpu, 0, 0), "readPixels from the GPU surface");

  sk_sp<SkSurface> cpu = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
  Draw(cpu->getCanvas(), typeface, image);
  SkBitmap from_cpu;
  from_cpu.allocN32Pixels(w, h);
  cpu->readPixels(from_cpu, 0, 0);

  check(from_gpu.getColor(30, 30) == SK_ColorMAGENTA && from_gpu.getColor(4, 4) == SK_ColorWHITE,
        "GPU: rectangle");
  check(from_gpu.getColor(96, 32) == SK_ColorBLUE, "GPU: antialiased circle");
  check(from_gpu.getColor(140, 12) == SK_ColorRED && from_gpu.getColor(164, 12) == SK_ColorGREEN &&
            from_gpu.getColor(140, 36) == SK_ColorBLUE &&
            from_gpu.getColor(164, 36) == SK_ColorYELLOW,
        "GPU: bitmap scaled 2x");
  int text_gpu = 0, text_cpu = 0, differ = 0, distant = 0, worst = 0;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      SkColor g = from_gpu.getColor(x, y), c = from_cpu.getColor(x, y);
      int d = Diff(g, c);
      worst = std::max(worst, d);
      if (d > 2) differ++;
      if (d > 64) distant++;
      if (y >= 60 && SkColorGetR(g) < 128) text_gpu++;
      if (y >= 60 && SkColorGetR(c) < 128) text_cpu++;
    }
  }
  std::printf("INFO  GPU vs CPU: %d of %d pixels differ by more than 2 (worst %d, %d by more than "
              "64); text pixels %d GPU, %d CPU\n",
              differ, w * h, worst, distant, text_gpu, text_cpu);
  check(text_gpu > 150 && std::abs(text_gpu - text_cpu) <= text_cpu / 10, "GPU: text drawn");
  check(differ <= w * h / 50 && distant <= w * h / 500, "GPU frame matches the CPU raster frame");

  gr->flushAndSubmit(GrSyncCpu::kYes);
  gpu.reset();
  gr.reset();
  EGL(eglMakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  EGL(eglDestroySurface)(display, pbuffer);
  EGL(eglDestroyContext)(display, context);
  EGL(eglTerminate)(display);

  if (failures == 0) {
    std::printf("all passed (Ganesh GL on ANGLE, D3D11 %s)\n", device);
  } else {
    std::printf("%d failed\n", failures);
  }
  return failures ? 1 : 0;
}
