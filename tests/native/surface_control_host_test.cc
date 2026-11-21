// gesher: AHardwareBuffer and ASurfaceControl from an NDK library into a
// Gesher window, without Java (ADR 0089).
//
//   surface_control_host_test <surface_control_test.so>
//
// Makes a window with gesher_window_create (compat/hwui/window), loads the
// library (tests/native/surface_control_test.c) through the ELF loader and the
// bionic shim, and has it run its AHardwareBuffer, EGL and SurfaceControl
// checks (each prints a line). Its layers end up composed into the window:
// PrintWindow reads the frame back and checks it against what the transactions
// asked for (kProbes); then the library takes its layers off the window, which
// gets an empty (transparent, read back black) frame. The device is ANGLE's
// default (GESHER_EGL_DEVICE=warp forces WARP); with GESHER_BINDER set the
// library also sends a buffer over an AF_UNIX socket through binderd's
// descriptor broker.
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

#if defined(__x86_64__)
#define SYSV __attribute__((sysv_abi))
#elif defined(__aarch64__)
#define SYSV
#endif
using Run = int(SYSV*)(ANativeWindow* window, char* out, size_t n);
using Finish = int(SYSV*)(char* out, size_t n);

constexpr int kWidth = 160, kHeight = 120;

struct Image {
  int width = 0, height = 0;
  std::vector<uint32_t> pixels;  // 0x00RRGGBB
  uint32_t at(int x, int y) const { return pixels[static_cast<size_t>(y) * width + x]; }
};

bool Capture(HWND hwnd, Image* image) {
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
    ok = PrintWindow(hwnd, dc, PW_CLIENTONLY) != 0;  // the window paints its back buffer
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

// What the library's first transaction composes (surface_control_test.c):
// an opaque blue background; the 32x16 quadrant buffer (red, green / magenta,
// white) rotated 90 degrees into (100, 20)-(132, 84), so magenta, red / white,
// green; a white color layer cropped to (10, 100)-(30, 110); no yellow (a
// layer below the opaque background, a hidden one).
struct Probe {
  int x, y;
  uint32_t rgb;
  const char* what;
};
const Probe kProbes[] = {
    {50, 60, 0x0000ff, "background"},
    {5, 5, 0x0000ff, "background corner"},
    {108, 36, 0xff00ff, "overlay top left (source bottom left)"},
    {124, 36, 0xff0000, "overlay top right (source top left)"},
    {108, 68, 0xffffff, "overlay bottom left (source bottom right)"},
    {124, 68, 0x00ff00, "overlay bottom right (source top right)"},
    {20, 105, 0xffffff, "color layer"},
    {35, 105, 0x0000ff, "beside the color layer's crop"},
    {140, 100, 0x0000ff, "beside the overlay"},
};

bool CheckComposed(HWND hwnd, std::string* seen) {
  Image image;
  if (!Capture(hwnd, &image) || image.width < kWidth || image.height < kHeight) {
    *seen = "no capture";
    return false;
  }
  int wrong = 0, yellow = 0;
  std::string text;
  for (const Probe& p : kProbes) {
    uint32_t got = image.at(p.x, p.y);
    if (got != p.rgb) {
      wrong++;
      char line[160];
      std::snprintf(line, sizeof line, " %s at (%d, %d): %06x, expected %06x;", p.what, p.x, p.y, got, p.rgb);
      text += line;
    }
  }
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) yellow += image.at(x, y) == 0xffff00;
  }
  char summary[96];
  std::snprintf(summary, sizeof summary, "%d probe(s) wrong, %d yellow pixel(s)", wrong, yellow);
  *seen = summary + text;
  return wrong == 0 && yellow == 0;
}

bool CheckEmpty(HWND hwnd, std::string* seen) {
  Image image;
  if (!Capture(hwnd, &image) || image.width < kWidth || image.height < kHeight) {
    *seen = "no capture";
    return false;
  }
  int other = 0;
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) other += image.at(x, y) != 0;
  }
  char text[64];
  std::snprintf(text, sizeof text, "%d pixel(s) not black", other);
  *seen = text;
  return other == 0;
}

template <typename F>
bool Retry(F check, std::string* seen) {
  for (int attempt = 0; attempt < 30; ++attempt) {
    if (check(seen)) return true;
    Sleep(100);
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: surface_control_host_test <surface_control_test.so>\n");
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
  auto run = reinterpret_cast<Run>(gesher::elf::Lookup(lib, "sc_test_run"));
  auto finish = reinterpret_cast<Finish>(gesher::elf::Lookup(lib, "sc_test_finish"));
  if (run == nullptr || finish == nullptr) {
    std::printf("FAIL: the library lacks sc_test_run/sc_test_finish\n");
    return 1;
  }
  ANativeWindow* window = gesher_window_create("surface_control_test", kWidth, kHeight);
  if (window == nullptr) {
    std::printf("FAIL: gesher_window_create\n");
    return 1;
  }
  gesher_window_show(window);
  HWND hwnd = static_cast<HWND>(gesher_window_hwnd(window));
  int failures = 0;
  char out[256];
  int rc = run(window, out, sizeof out);
  std::printf("sc_test_run: %s\n", out);
  if (rc != 0) failures++;

  std::string seen;
  bool composed = Retry([&](std::string* s) { return CheckComposed(hwnd, s); }, &seen);
  std::printf("%s: the composed layers read back from the window: %s\n", composed ? "ok" : "FAIL",
              seen.c_str());
  if (!composed) failures++;

  rc = finish(out, sizeof out);
  std::printf("sc_test_finish: %s\n", out);
  if (rc != 0) failures++;
  bool empty = Retry([&](std::string* s) { return CheckEmpty(hwnd, s); }, &seen);
  std::printf("%s: the window after its layers left: %s\n", empty ? "ok" : "FAIL", seen.c_str());
  if (!empty) failures++;
  ANativeWindow_release(window);

  if (failures == 0) {
    std::printf("surface_control_test all passed\n");
    return 0;
  }
  std::printf("surface_control_test: %d failure(s)\n", failures);
  return 1;
}
