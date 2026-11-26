// Host-only observation of the real Win32Surface implementation. Poison bytes
// outside declared damage make an unintended full copy observable at Window.
namespace surface_damage_contract {
using surface_contract::Check;
using gesher::PixelRect;
uint32_t Convert(uint32_t pixel, int format) {
  if (format == HAL_PIXEL_FORMAT_BGRA_8888) return pixel;
  const uint32_t converted = (pixel & 0xff00ff00u) | ((pixel & 0xffu) << 16) | ((pixel >> 16) & 0xffu);
  return format == HAL_PIXEL_FORMAT_RGBX_8888 ? converted | 0xff000000u : converted;
}
void Pixels(gesher::Window* window, const std::vector<uint32_t>& expected, const char* reason) {
  std::vector<uint32_t> actual;
  int w, h;
  window->ReadBackBuffer(&actual, &w, &h);
  Check(actual == expected, reason);
}
bool Rect(const PixelRect& a, int l, int t, int r, int b) {
  return a.left == l && a.top == t && a.right == r && a.bottom == b;
}
void FormatsAndDamage(int format) {
  constexpr int w = 8, h = 6;
  auto* window = new gesher::Window(w, h);
  auto surface = android::sp<gesher::Win32Surface>::make(window);
  auto* native = static_cast<ANativeWindow*>(surface.get());
  Check(native->perform(native, NATIVE_WINDOW_SET_BUFFERS_FORMAT, format) == 0, "damage format setup");
  ANativeWindow_Buffer buffer;
  ARect dirty{2, 1, 4, 3};
  Check(surface->lock(&buffer, &dirty) == 0, "damage first lock");
  Check(surface_contract::RectEquals(dirty, 0, 0, w, h), "first damage expands to full frame");
  constexpr uint32_t first = 0x7f123456, second = 0x4813579b, poison = 0x11223344;
  auto* pixels = static_cast<uint32_t*>(buffer.bits);
  std::fill_n(pixels, w * h, first);
  surface->unlockAndPost();
  std::vector<uint32_t> expected(w * h, Convert(first, format));
  Pixels(window, expected, "first frame converts every pixel");
  dirty = {2, 1, 4, 3};
  surface->lock(&buffer, &dirty);
  std::fill_n(pixels, w * h, poison);
  for (int y = 1; y < 3; ++y) for (int x = 2; x < 4; ++x) {
    pixels[y * w + x] = second;
    expected[y * w + x] = Convert(second, format);
  }
  surface->unlockAndPost();
  Pixels(window, expected, "partial conversion preserves destination outside dirty rows/columns");
  Check(Rect(window->last_damage_, 2, 1, 4, 3), "dirty rectangle reaches Window unchanged");

  // CPU clipping, including negative input, remains in producer coordinates.
  dirty = {-10, -20, 2, 1};
  surface->lock(&buffer, &dirty);
  pixels[0] = pixels[1] = first;
  expected[0] = expected[1] = Convert(first, format);
  surface->unlockAndPost();
  Pixels(window, expected, "clipped partial conversion");
  Check(Rect(window->last_damage_, 0, 0, 2, 1), "clipped damage reaches Window");

  // Another producer overwrites the destination. Its version invalidates the
  // optimization even when dimensions and the CPU source pointer are stable.
  std::vector<uint32_t> external(w * h, 0xffffffff);
  window->PresentPixels(external.data(), w, h);
  dirty = {2, 1, 4, 3};
  surface->lock(&buffer, &dirty);
  surface->unlockAndPost();
  for (int i = 0; i < w * h; ++i) expected[i] = Convert(pixels[i], format);
  Pixels(window, expected, "intervening writer forces full reconversion");
  Check(Rect(window->last_damage_, 0, 0, w, h), "intervening writer forces full invalidation");

  for (ARect invalid : {ARect{0, 0, 0, 0}, ARect{100, 100, 110, 110}, ARect{4, 4, 2, 2}}) {
    surface->lock(&buffer, &invalid);
    std::fill_n(pixels, w * h, second);
    surface->unlockAndPost();
    std::fill(expected.begin(), expected.end(), Convert(second, format));
    Pixels(window, expected, "empty/invalid damage safely falls back to full copy");
  }

  ANativeWindowBuffer* queued = nullptr;
  int fence;
  native->perform(native, NATIVE_WINDOW_API_DISCONNECT, NATIVE_WINDOW_API_CPU);
  native->perform(native, NATIVE_WINDOW_API_CONNECT, NATIVE_WINDOW_API_EGL);
  native->dequeueBuffer(native, &queued, &fence);
  pixels = static_cast<uint32_t*>(queued->reserved[0]);
  std::fill_n(pixels, w * h, poison);
  // Bottom-left rectangles union to [1,1..5,4) in top-left coordinates.
  const android_native_rect_t rectangles[] = {{1, 5, 3, 4}, {4, 3, 5, 2}};
  Check(native->perform(native, NATIVE_WINDOW_SET_SURFACE_DAMAGE, rectangles, size_t{2}) == 0,
        "queued damage accepts two bottom-left rectangles");
  for (int y = 1; y < 4; ++y) for (int x = 1; x < 5; ++x) expected[y * w + x] = Convert(poison, format);
  native->queueBuffer(native, queued, fence);
  Pixels(window, expected, "queued damage uses correct bottom-left union");
  Check(Rect(window->last_damage_, 1, 1, 5, 4), "queued damage union reaches Window");
  native->dequeueBuffer(native, &queued, &fence);
  native->queueBuffer(native, queued, fence);
  std::fill(expected.begin(), expected.end(), Convert(poison, format));
  Pixels(window, expected, "queue resets damage to full for next frame");
  Check(native->perform(native, NATIVE_WINDOW_SET_SURFACE_DAMAGE,
                        static_cast<const android_native_rect_t*>(nullptr), size_t{1}) == -EINVAL,
        "nonzero damage count with null rectangles rejected");
  native->perform(native, NATIVE_WINDOW_API_DISCONNECT, NATIVE_WINDOW_API_EGL);
  native->perform(native, NATIVE_WINDOW_API_CONNECT, NATIVE_WINDOW_API_CPU);
  native->dequeueBuffer(native, &queued, &fence);
  pixels = static_cast<uint32_t*>(queued->reserved[0]);
  std::fill_n(pixels, w * h, first);
  native->perform(native, NATIVE_WINDOW_SET_SURFACE_DAMAGE, rectangles, size_t{2});
  native->queueBuffer(native, queued, fence);
  std::fill(expected.begin(), expected.end(), Convert(first, format));
  Pixels(window, expected, "CPU connection ignores queued surface damage as AOSP does");
  Check(Rect(window->last_damage_, 0, 0, w, h), "CPU queued frame remains fully damaged");

  // Resize away and back without a CPU post: same dimensions are insufficient
  // evidence that a destination retained our last frame.
  window->Resize(2, 2);
  window->Resize(w, h);
  dirty = {2, 1, 3, 2};
  surface->lock(&buffer, &dirty);
  surface->unlockAndPost();
  Pixels(window, expected, "resize away/back forces complete retained-image restoration");
  Check(Rect(window->last_damage_, 0, 0, w, h), "resize generation invalidates damage optimization");
  native->dequeueBuffer(native, &queued, &fence);
  std::fill_n(static_cast<uint32_t*>(queued->reserved[0]), w * h, first);
  native->cancelBuffer(native, queued, fence);
  dirty = {2, 1, 3, 2};
  surface->lock(&buffer, &dirty);
  Check(surface_contract::RectEquals(dirty, 0, 0, w, h), "cancelled producer buffer requires a complete redraw");
  std::fill_n(static_cast<uint32_t*>(buffer.bits), w * h, second);
  surface->unlockAndPost();
  std::fill(expected.begin(), expected.end(), Convert(second, format));
  Pixels(window, expected, "cancelled-buffer restoration publishes a complete frame");
}
void Insets() {
  auto* window = new gesher::Window(4, 3);
  auto surface = android::sp<gesher::Win32Surface>::make(window);
  surface->SetSurfaceInsets(1, 2, 2, 1);
  ANativeWindow_Buffer buffer;
  surface->lock(&buffer, nullptr);
  auto* pixels = static_cast<uint32_t*>(buffer.bits);
  std::fill_n(pixels, buffer.stride * buffer.height, 0xff112233);
  surface->unlockAndPost();
  std::vector<uint32_t> expected(12, 0xff332211);
  ARect dirty{2, 3, 4, 4};
  surface->lock(&buffer, &dirty);
  std::fill_n(pixels, buffer.stride * buffer.height, 0xff445566);
  expected[5] = expected[6] = 0xff665544;
  surface->unlockAndPost();
  Pixels(window, expected, "inset damage converts producer coordinates to client coordinates");
  Check(Rect(window->last_damage_, 1, 1, 3, 2), "inset damage reaches Window in client pixels");
  surface->SetSurfaceInsets(0, 0, 0, 0);
  dirty = {1, 1, 2, 2};
  surface->lock(&buffer, &dirty);
  Check(surface_contract::RectEquals(dirty, 0, 0, 4, 3), "inset-induced buffer resize forces full redraw");
  surface->unlockAndPost();
}
void Run() {
  const int before = surface_contract::checks;
  for (int format : {HAL_PIXEL_FORMAT_RGBA_8888, HAL_PIXEL_FORMAT_RGBX_8888, HAL_PIXEL_FORMAT_BGRA_8888})
    FormatsAndDamage(format);
  Insets();
  std::printf("surface damage contracts: %d checks, %d cumulative failures\n",
              surface_contract::checks - before, surface_contract::failures);
}
void Measure() {
  constexpr int w = 1920, h = 1080, frames = 400;
  for (bool partial : {false, true}) {
    auto* window = new gesher::Window(w, h);
    auto surface = android::sp<gesher::Win32Surface>::make(window);
    ANativeWindow_Buffer buffer;
    surface->lock(&buffer, nullptr);
    std::fill_n(static_cast<uint32_t*>(buffer.bits), w * h, 0xff123456);
    surface->unlockAndPost();
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < frames; ++frame) {
      ARect dirty{100, 100, 164, 164};
      surface->lock(&buffer, partial ? &dirty : nullptr);
      auto* pixels = static_cast<uint32_t*>(buffer.bits);
      for (int y = 100; y < 164; ++y)
        std::fill_n(pixels + y * buffer.stride + 100, 64, 0xff123456u + frame);
      surface->unlockAndPost();
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::vector<uint32_t> shown;
    int width, height;
    window->ReadBackBuffer(&shown, &width, &height);
    std::printf("Surface damage 1920x1080 %s: frames=%d elapsed_ms=%.3f checksum=%08x\n",
                partial ? "64x64" : "full", frames, ms, shown[100 * w + 100]);
  }
}
}  // namespace surface_damage_contract
