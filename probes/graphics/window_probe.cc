// Gesher's Win32 window (compat/hwui/window): Skia draws into its back
// buffer, PrintWindow reads the window back (no visible desktop needed), and
// mouse and key messages come out of its event queue; then Win32Surface, the
// ANativeWindow over a window: Skia draws RGBA (Android's order) through
// ANativeWindow_lock and through dequeueBuffer/queueBuffer (hwui's CPU
// pipeline), and the window shows it. Run in CI on Windows
// (tests/windows/tracks/30-graphics.ps1); prints "all passed" when every check
// holds, and writes what PrintWindow read to window_probe.png.
//
//   window_probe [out.png]
#undef NOGDI
#include <windows.h>

#include <sys/eventfd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <system/window.h>

#include "gesher_native_window.h"
#include "gesher_window.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"

static int failures = 0;

static void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  std::fflush(stdout);
  if (!ok) failures++;
}

static bool near(uint32_t bgra, SkColor want) {
  int b = bgra & 0xff, g = (bgra >> 8) & 0xff, r = (bgra >> 16) & 0xff;
  return std::abs(r - int(SkColorGetR(want))) <= 8 && std::abs(g - int(SkColorGetG(want))) <= 8 &&
         std::abs(b - int(SkColorGetB(want))) <= 8;
}

static void draw(gesher::Window* window) {
  int w, h, stride;
  uint8_t* pixels = window->Lock(&w, &h, &stride);
  SkImageInfo info = SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
  auto canvas = SkCanvas::MakeRasterDirect(info, pixels, static_cast<size_t>(stride) * 4);
  canvas->clear(SK_ColorWHITE);
  SkPaint paint;
  paint.setColor(SK_ColorRED);
  canvas->drawRect(SkRect::MakeXYWH(0, 0, w / 2.0f, h), paint);
  paint.setColor(SK_ColorBLUE);
  canvas->drawRect(SkRect::MakeXYWH(w / 2.0f, 0, w / 2.0f, h), paint);
  paint.setColor(SK_ColorGREEN);
  paint.setAntiAlias(true);
  canvas->drawCircle(w / 2.0f, h / 2.0f, 30, paint);
  canvas.reset();
  window->UnlockAndPost();
}

// What PrintWindow draws of the client area, as BGRA rows.
static bool capture(HWND hwnd, int w, int h, std::vector<uint32_t>* out) {
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
  HGDIOBJ old = SelectObject(dc, bmp);
  std::memset(bits, 0, static_cast<size_t>(w) * h * 4);
  BOOL ok = PrintWindow(hwnd, dc, PW_CLIENTONLY);
  GdiFlush();
  out->assign(static_cast<uint32_t*>(bits), static_cast<uint32_t*>(bits) + w * h);
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
  ReleaseDC(nullptr, screen);
  return ok;
}

// What WM_PRINTCLIENT paints into a memory DC: the back buffer, whatever the
// desktop (PrintWindow goes through the window's paint path too).
static void printClient(HWND hwnd, int w, int h, std::vector<uint32_t>* out) {
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
  HGDIOBJ old = SelectObject(dc, bmp);
  std::memset(bits, 0, static_cast<size_t>(w) * h * 4);
  SendMessageW(hwnd, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
  GdiFlush();
  out->assign(static_cast<uint32_t*>(bits), static_cast<uint32_t*>(bits) + w * h);
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
  ReleaseDC(nullptr, screen);
}

static bool waitFor(gesher::Window* window, int type, gesher::WindowEvent* e) {
  for (int i = 0; i < 50; i++) {
    if (!window->WaitEvent(e, 100)) continue;
    if (e->type == type) return true;
  }
  return false;
}

// Win32Surface through the NDK's functions, as libandroid's callers and
// android.view.Surface's natives use it.
static void surfaceChecks() {
  const int kW = 200, kH = 100;
  ANativeWindow* anw = gesher_window_create("Gesher surface probe", kW, kH);
  HWND hwnd = anw != nullptr ? static_cast<HWND>(gesher_window_hwnd(anw)) : nullptr;
  check(hwnd != nullptr, "surface: gesher_window_create");
  if (hwnd == nullptr) return;
  gesher_window_show(anw);
  check(ANativeWindow_getWidth(anw) == kW && ANativeWindow_getHeight(anw) == kH &&
            ANativeWindow_getFormat(anw) == WINDOW_FORMAT_RGBA_8888,
        "surface: 200x100, RGBA_8888");

  // Software drawing (Surface.lockCanvas, ANativeWindow_lock): RGBA bytes.
  ANativeWindow_Buffer buffer;
  ARect dirty = {0, 0, 0, 0};
  bool locked = ANativeWindow_lock(anw, &buffer, &dirty) == 0;
  check(locked && buffer.width == kW && buffer.height == kH && buffer.stride >= kW &&
            buffer.format == WINDOW_FORMAT_RGBA_8888 && dirty.right == kW && dirty.bottom == kH,
        "surface: ANativeWindow_lock (whole buffer dirty)");
  if (!locked) return;
  const SkColor kColor = SkColorSetRGB(0x20, 0x60, 0xc0);
  {
    SkImageInfo info = SkImageInfo::Make(kW, kH, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    auto canvas = SkCanvas::MakeRasterDirect(info, buffer.bits, buffer.stride * 4);
    canvas->clear(SK_ColorWHITE);
    SkPaint paint;
    paint.setColor(SK_ColorRED);
    canvas->drawRect(SkRect::MakeXYWH(0, 0, kW / 2.0f, kH), paint);
    paint.setColor(kColor);
    canvas->drawRect(SkRect::MakeXYWH(kW / 2.0f, 0, kW / 2.0f, kH), paint);
  }
  check(ANativeWindow_unlockAndPost(anw) == 0, "surface: ANativeWindow_unlockAndPost");
  Sleep(200);
  std::vector<uint32_t> px;
  bool printed = capture(hwnd, kW, kH, &px);
  check(printed && near(px[10 * kW + 10], SK_ColorRED) && near(px[10 * kW + kW - 10], kColor),
        "surface: the window shows the RGBA frame (red, 0x2060c0)");

  // hwui's CPU pipeline: dequeueBuffer's reserved[0] is the pixels it draws
  // into; queueBuffer presents them.
  ANativeWindowBuffer* b = nullptr;
  int fence = 0;
  bool dequeued = anw->dequeueBuffer(anw, &b, &fence) == 0 && b != nullptr;
  check(dequeued && b->width == kW && b->height == kH && b->reserved[0] != nullptr && fence == -1,
        "surface: dequeueBuffer");
  if (!dequeued) return;
  {
    SkImageInfo info = SkImageInfo::Make(kW, kH, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    auto canvas = SkCanvas::MakeRasterDirect(info, b->reserved[0], b->stride * 4);
    canvas->clear(SK_ColorGREEN);
  }
  check(anw->queueBuffer(anw, b, -1) == 0, "surface: queueBuffer");
  Sleep(200);
  printed = capture(hwnd, kW, kH, &px);
  check(printed && near(px[50 * kW + 100], SK_ColorGREEN), "surface: the window shows the queued buffer");

  ANativeWindow_release(anw);
  check(!IsWindow(hwnd), "surface: the window goes with the last reference");
}

int main(int argc, char** argv) {
  const char* png = argc > 1 ? argv[1] : "window_probe.png";
  const int kW = 320, kH = 240;
  gesher::Window* window = gesher::Window::Create("Gesher window probe", kW, kH);
  check(window != nullptr && window->hwnd() != nullptr, "window: create a top-level window");
  if (window == nullptr) return 1;
  HWND hwnd = static_cast<HWND>(window->hwnd());
  window->Show();
  check(window->width() == kW && window->height() == kH, "window: client area is 320x240");

  draw(window);
  Sleep(200);
  std::vector<uint32_t> px;
  bool printed = capture(hwnd, kW, kH, &px);
  check(printed, "window: PrintWindow");
  check(near(px[10 * kW + 10], SK_ColorRED) && near(px[10 * kW + kW - 10], SK_ColorBLUE) &&
            near(px[(kH / 2) * kW + kW / 2], SK_ColorGREEN),
        "window: PrintWindow reads back what Skia drew (red, blue, green)");
  std::vector<uint32_t> pc;
  printClient(hwnd, kW, kH, &pc);
  check(near(pc[10 * kW + 10], SK_ColorRED) && near(pc[10 * kW + kW - 10], SK_ColorBLUE),
        "window: WM_PRINTCLIENT paints the back buffer");
  {
    // What PrintWindow read, for the CI artifact.
    SkImageInfo info = SkImageInfo::Make(kW, kH, kBGRA_8888_SkColorType, kOpaque_SkAlphaType);
    SkPixmap pm(info, px.data(), kW * 4);
    SkFILEWStream file(png);
    SkPngEncoder::Encode(&file, pm, {});
  }

  // Input: messages posted to the window come out of the queue, in client
  // coordinates, and the eventfd says so.
  gesher::WindowEvent e;
  while (window->PollEvent(&e)) {
  }
  PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 50));
  PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(40, 50));
  Sleep(100);
  eventfd_t count = 0;
  check(eventfd_read(window->event_fd(), &count) == 0 && count > 0,
        "window: the eventfd is readable while events are queued");
  bool down = waitFor(window, gesher::WindowEvent::kMouseDown, &e);
  check(down && e.x == 40 && e.y == 50 && e.button == 1, "window: left button down at (40, 50)");
  bool up = waitFor(window, gesher::WindowEvent::kMouseUp, &e);
  check(up && e.x == 40 && e.y == 50 && e.button == 1, "window: left button up at (40, 50)");
  PostMessageW(hwnd, WM_KEYDOWN, 'A', 0x001e0001);
  PostMessageW(hwnd, WM_CHAR, 'a', 0x001e0001);
  PostMessageW(hwnd, WM_KEYUP, 'A', 0xc01e0001);
  bool kd = waitFor(window, gesher::WindowEvent::kKeyDown, &e);
  check(kd && e.key == 'A' && e.scan == 0x1e, "window: key down (A)");
  bool ch = waitFor(window, gesher::WindowEvent::kChar, &e);
  check(ch && e.ch == 'a', "window: character (a)");
  bool ku = waitFor(window, gesher::WindowEvent::kKeyUp, &e);
  check(ku && e.key == 'A', "window: key up (A)");
  PostMessageW(hwnd, WM_CLOSE, 0, 0);
  check(waitFor(window, gesher::WindowEvent::kClose, &e) && IsWindow(hwnd),
        "window: close is an event; the window stays until the app destroys it");

  // A resize reaches the app, and the next frame has the new size.
  SetWindowPos(hwnd, nullptr, 0, 0, 500, 400, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  bool resized = waitFor(window, gesher::WindowEvent::kResize, &e);
  check(resized && e.width == window->width() && e.height == window->height() && e.width > kW,
        "window: resize is an event with the new client size");
  int w, h, stride;
  window->Lock(&w, &h, &stride);
  window->Unlock();
  check(w == window->width() && h == window->height(), "window: the back buffer follows the size");

  delete window;
  check(!IsWindow(hwnd), "window: destroyed");

  surfaceChecks();
  if (failures == 0) std::printf("all passed\n");
  return failures == 0 ? 0 : 1;
}
