// gesher: an NDK library drawing into Gesher windows, without Java
// (docs/native-libs.md, "Graphics").
//
//   gl_window_test <libglapp.so>
//
// Makes windows with gesher_window_create (compat/hwui/window), loads
// libglapp.so (tests/apps/glapp/jni/glapp.c) through the ELF loader and the
// bionic shim, and has it draw:
//  - with ANativeWindow_lock/unlockAndPost: the window is read back with
//    PrintWindow (PW_CLIENTONLY, which repaints the back buffer into a memory
//    DC) and its four quadrants checked;
//  - with EGL/GLES 3 and then 2 (eglCreateWindowSurface on the window, a
//    triangle, glReadPixels checked by the library, eglSwapBuffers): the
//    window is then read back from what DWM composed (PrintWindow with
//    PW_RENDERFULLCONTENT, which sees the swap chain's frame) and its center,
//    corners and green area checked.
// The device is ANGLE's default (hardware, else WARP; GESHER_EGL_DEVICE=warp
// forces WARP).
#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>

#include "gesher_native_window.h"

// The project defines NOGDI for AOSP code; this test reads windows back with GDI.
#undef NOGDI
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace {

#define SYSV __attribute__((sysv_abi))
using CpuDraw = int(SYSV*)(ANativeWindow* window, char* out, size_t n);
using GlDraw = int(SYSV*)(ANativeWindow* window, int es, char* out, size_t n);
using GlFinish = void(SYSV*)();

// Wider than the narrowest client area Windows gives a captioned window
// (about 120 pixels on the runners).
constexpr int kWidth = 160, kHeight = 96;

struct Image {
  int width = 0, height = 0;
  std::vector<uint32_t> pixels;  // 0x00RRGGBB (alpha cleared)
  uint32_t at(int x, int y) const { return pixels[static_cast<size_t>(y) * width + x]; }
};

// The client area into a top-down 32-bit DIB: with PrintWindow(flags), or
// copied from the screen where the window's client area is (from_screen).
bool Capture(HWND hwnd, UINT flags, bool from_screen, Image* image) {
  RECT rc;
  if (!GetClientRect(hwnd, &rc)) return false;
  int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w < 1 || h < 1) return false;
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  bool ok = false;
  if (bmp != nullptr) {
    HGDIOBJ old = SelectObject(dc, bmp);
    std::memset(bits, 0, static_cast<size_t>(w) * h * 4);
    if (from_screen) {
      POINT origin = {0, 0};
      ClientToScreen(hwnd, &origin);
      ok = BitBlt(dc, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY) != 0;
    } else {
      ok = PrintWindow(hwnd, dc, flags) != 0;
    }
    GdiFlush();
    const uint32_t* p = static_cast<const uint32_t*>(bits);
    image->pixels.assign(p, p + static_cast<size_t>(w) * h);
    for (uint32_t& px : image->pixels) px &= 0xffffffu;
    image->width = w;
    image->height = h;
    SelectObject(dc, old);
    DeleteObject(bmp);
  }
  DeleteDC(dc);
  ReleaseDC(nullptr, screen);
  return ok;
}

// What glapp_cpu_draw draws: red, green / blue, white quadrants.
int WrongQuadrantPixels(const Image& image) {
  static const uint32_t kColors[4] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff};
  int wrong = 0;
  for (int y = 0; y < image.height; y++) {
    for (int x = 0; x < image.width; x++) {
      int q = (y >= image.height / 2 ? 2 : 0) + (x >= image.width / 2 ? 1 : 0);
      if (image.at(x, y) != kColors[q]) wrong++;
    }
  }
  return wrong;
}

// What glapp_gl_draw draws: a green triangle in the middle of blue.
bool LooksLikeTriangle(const Image& image, std::string* seen) {
  int w = image.width, h = image.height;
  int green = 0, blue = 0;
  for (uint32_t p : image.pixels) {
    if (p == 0x00ff00) green++;
    if (p == 0x0000ff) blue++;
  }
  char text[160];
  std::snprintf(text, sizeof text, "center %06x, corners %06x %06x %06x %06x, %d green, %d blue",
                image.at(w / 2, h / 2), image.at(0, 0), image.at(w - 1, 0), image.at(0, h - 1),
                image.at(w - 1, h - 1), green, blue);
  *seen = text;
  int expected = w * h / 8;
  return image.at(w / 2, h / 2) == 0x00ff00 && image.at(0, 0) == 0x0000ff &&
         image.at(w - 1, 0) == 0x0000ff && image.at(0, h - 1) == 0x0000ff &&
         image.at(w - 1, h - 1) == 0x0000ff && green > expected * 8 / 10 &&
         green < expected * 12 / 10;
}

// Reads the GL window back the ways that see what the swap chain presented;
// DWM composes asynchronously, so each is tried for up to 2 s.
bool CheckGlWindow(HWND hwnd, const char* label) {
  struct Way {
    const char* name;
    UINT flags;
    bool from_screen;
  };
  // (Copying the screen under the window does not see it on the CI
  // runners, whose desktop is not composed for display.)
  const Way kWays[] = {{"PrintWindow(PW_RENDERFULLCONTENT)", PW_CLIENTONLY | PW_RENDERFULLCONTENT, false}};
  bool any = false;
  for (const Way& way : kWays) {
    std::string seen = "nothing";
    bool ok = false;
    for (int attempt = 0; attempt < 20 && !ok; attempt++) {
      Image image;
      if (Capture(hwnd, way.flags, way.from_screen, &image)) ok = LooksLikeTriangle(image, &seen);
      if (!ok) Sleep(100);
    }
    std::printf("%s window read back from %s: %s (%s)\n", label, way.name,
                ok ? "triangle ok" : "not matched", seen.c_str());
    any = any || ok;
  }
  std::fflush(stdout);
  return any;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: gl_window_test <libglapp.so>\n");
    return 2;
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  gesher::bionic::InstallBionicShim();
  gesher::elf::SetupThreadTls();
  std::string error;
  gesher::elf::Library* lib = gesher::elf::Load(argv[1], &error);
  if (lib == nullptr) {
    std::printf("FAIL: load %s: %s\n", argv[1], error.c_str());
    return 1;
  }
  auto cpu_draw = reinterpret_cast<CpuDraw>(gesher::elf::Lookup(lib, "glapp_cpu_draw"));
  auto gl_draw = reinterpret_cast<GlDraw>(gesher::elf::Lookup(lib, "glapp_gl_draw"));
  auto gl_finish = reinterpret_cast<GlFinish>(gesher::elf::Lookup(lib, "glapp_gl_finish"));
  if (cpu_draw == nullptr || gl_draw == nullptr || gl_finish == nullptr) {
    std::printf("FAIL: libglapp.so lacks its exports\n");
    return 1;
  }
  int failures = 0;
  char out[1024];

  // ANativeWindow_lock into a window, read back with PrintWindow.
  ANativeWindow* cpu_window = gesher_window_create("gl_window_test: ANativeWindow_lock", kWidth, kHeight);
  if (cpu_window == nullptr) {
    std::printf("FAIL: gesher_window_create\n");
    return 1;
  }
  gesher_window_show(cpu_window);
  int rc = cpu_draw(cpu_window, out, sizeof out);
  std::printf("cpu: %s\n", out);
  if (rc != 0) failures++;
  Image cpu_image;
  if (!Capture(static_cast<HWND>(gesher_window_hwnd(cpu_window)), PW_CLIENTONLY, false, &cpu_image)) {
    std::printf("FAIL: PrintWindow of the cpu window\n");
    failures++;
  } else {
    int wrong = WrongQuadrantPixels(cpu_image);
    std::printf("cpu window read back with PrintWindow: %dx%d, %d pixels not red/green/blue/white by quadrant%s\n",
                cpu_image.width, cpu_image.height, wrong, wrong == 0 ? ": quadrants ok" : "");
    if (wrong != 0) failures++;
  }
  ANativeWindow_release(cpu_window);

  // EGL/GLES 3 and 2, each on a window of its own.
  bool composed = true;
  for (int es : {3, 2}) {
    std::string title = "gl_window_test: GLES " + std::to_string(es);
    ANativeWindow* window = gesher_window_create(title.c_str(), kWidth, kHeight);
    if (window == nullptr) {
      std::printf("FAIL: gesher_window_create\n");
      return 1;
    }
    gesher_window_show(window);
    rc = gl_draw(window, es, out, sizeof out);
    std::printf("es%d: %s\n", es, out);
    if (rc != 0) {
      failures++;
    } else {
      char label[16];
      std::snprintf(label, sizeof label, "es%d", es);
      composed = CheckGlWindow(static_cast<HWND>(gesher_window_hwnd(window)), label) && composed;
    }
    gl_finish();
    ANativeWindow_release(window);
  }

  if (composed && failures == 0) std::printf("gl_window_test: GL windows read back from Windows\n");
  if (failures == 0) {
    std::printf("gl_window_test all passed\n");
    return 0;
  }
  std::printf("gl_window_test: %d failure(s)\n", failures);
  return 1;
}
