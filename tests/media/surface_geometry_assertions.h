// Shared behavioral assertions: host adapters and actual Windows surface implementations.
namespace surface_contract {
int checks = 0, failures = 0;
bool Check(bool ok, const char* reason) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL surface geometry: %s\n", reason); }
  return ok;
}
int Query(ANativeWindow* window, int what) {
  int value = -999;
  Check(window->query(window, what, &value) == 0, "native-window query succeeds");
  return value;
}
bool RectEquals(const ARect& rect, int left, int top, int right, int bottom) {
  return rect.left == left && rect.top == top && rect.right == right && rect.bottom == bottom;
}
void BufferSize(ANativeWindow* window, int width, int height) {
  ANativeWindowBuffer* buffer = nullptr;
  int fence = -1;
  if (!Check(window->dequeueBuffer(window, &buffer, &fence) == 0 && buffer != nullptr,
             "dequeue geometry buffer")) return;
  Check(buffer->width == width && buffer->height == height, "dequeue follows requested/user/default precedence");
  Check(window->cancelBuffer(window, buffer, fence) == 0, "cancel inspected buffer");
}
void Run(android::Surface* surface, int width, int height) {
  ANativeWindow* window = surface;
  Check(Query(window, NATIVE_WINDOW_WIDTH) == width && Query(window, NATIVE_WINDOW_HEIGHT) == height,
        "consumer geometry is the initial window geometry");
  BufferSize(window, width, height);
  Check(window->perform(window, NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS, 8, 10) == 0, "set user geometry");
  Check(Query(window, NATIVE_WINDOW_DEFAULT_WIDTH) == 8 && Query(window, NATIVE_WINDOW_DEFAULT_HEIGHT) == 10,
        "DEFAULT queries reflect user geometry");
  Check(Query(window, NATIVE_WINDOW_WIDTH) == width && Query(window, NATIVE_WINDOW_HEIGHT) == height,
        "user buffer override does not change consumer query geometry");
  BufferSize(window, 8, 10);
  Check(window->perform(window, NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS, 12, 14) == 0, "set producer geometry");
  BufferSize(window, 12, 14);
  Check(Query(window, NATIVE_WINDOW_DEFAULT_WIDTH) == 8 && Query(window, NATIVE_WINDOW_DEFAULT_HEIGHT) == 10,
        "producer dimensions do not overwrite user defaults");
  Check(window->perform(window, NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS, 0, 14) == -EINVAL,
        "mixed-zero producer geometry rejected");
  Check(window->perform(window, NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS, 8, 0) == -EINVAL,
        "mixed-zero user geometry rejected");
  BufferSize(window, 12, 14);
  Check(window->perform(window, NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS, 0, 0) == 0, "clear producer override");
  BufferSize(window, 8, 10);
  Check(window->perform(window, NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS, 0, 0) == 0, "clear user override");
  BufferSize(window, width, height);

  // Legacy geometry sets producer size and then format; zero format resets to default.
  Check(window->perform(window, NATIVE_WINDOW_SET_BUFFERS_FORMAT, HAL_PIXEL_FORMAT_BGRA_8888) == 0,
        "set alternate supported format");
  Check(window->perform(window, NATIVE_WINDOW_SET_BUFFERS_GEOMETRY, 7, 9, 0) == 0,
        "legacy geometry accepts zero-format reset");
  Check(Query(window, NATIVE_WINDOW_FORMAT) == HAL_PIXEL_FORMAT_RGBA_8888, "zero format resets RGBA");
  Check(Query(window, NATIVE_WINDOW_DEFAULT_WIDTH) == width, "legacy producer geometry leaves user default alone");
  BufferSize(window, 7, 9);
  Check(window->perform(window, NATIVE_WINDOW_SET_BUFFERS_GEOMETRY, 0, 2, HAL_PIXEL_FORMAT_BGRA_8888) == -EINVAL,
        "legacy mixed-zero dimensions fail before changing format");
  Check(Query(window, NATIVE_WINDOW_FORMAT) == HAL_PIXEL_FORMAT_RGBA_8888, "failed dimensions retain format");
  BufferSize(window, 7, 9);
  Check(window->perform(window, NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS, 0, 0) == 0, "restore consumer-sized buffer");

  ANativeWindow_Buffer buffer = {};
  ARect dirty = {1, 1, 2, 2};
  if (!Check(surface->lock(&buffer, &dirty) == 0, "first CPU lock after allocation")) return;
  Check(RectEquals(dirty, 0, 0, width, height), "first lock invalidates full frame even after dequeues");
  std::fill_n(static_cast<uint32_t*>(buffer.bits), static_cast<size_t>(buffer.stride) * buffer.height, 0xffabcdef);
  Check(surface->unlockAndPost() == 0, "publish initial pixels");
  dirty = {-4, -3, width + 10, height - 1};
  if (!Check(surface->lock(&buffer, &dirty) == 0, "lock existing content")) return;
  Check(RectEquals(dirty, 0, 0, width, height - 1), "existing damage is intersected with buffer bounds");
  Check(static_cast<uint32_t*>(buffer.bits)[(height - 1) * buffer.stride] == 0xffabcdef,
        "untouched pixels survive partial redraw");
  Check(surface->unlockAndPost() == 0, "publish partial frame");
  dirty = {width + 3, height + 3, width + 4, height + 4};
  if (!Check(surface->lock(&buffer, &dirty) == 0, "lock out-of-bounds damage")) return;
  Check(RectEquals(dirty, 0, 0, 0, 0), "outside damage has empty intersection");
  Check(surface->unlockAndPost() == 0, "publish unchanged frame");
  dirty = {2, 2, 1, 1};
  if (!Check(surface->lock(&buffer, &dirty) == 0, "lock empty damage")) return;
  Check(RectEquals(dirty, 0, 0, 0, 0), "empty damage stays empty on preserved content");
  Check(surface->unlockAndPost() == 0, "publish empty-damage frame");

  Check(window->perform(window, NATIVE_WINDOW_SET_BUFFERS_FORMAT, HAL_PIXEL_FORMAT_BGRA_8888) == 0,
        "change pixel format at same dimensions");
  BufferSize(window, width, height);  // preallocation must not hide invalidation from lock.
  dirty = {1, 1, 2, 2};
  if (!Check(surface->lock(&buffer, &dirty) == 0, "lock reformatted buffer")) return;
  Check(RectEquals(dirty, 0, 0, width, height), "format change invalidates entire frame");
  Check(surface->unlockAndPost() == 0, "publish reformatted frame");
  Check(window->perform(window, NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS, 9, 11) == 0,
        "resize user buffer");
  BufferSize(window, 9, 11);
  dirty = {1, 1, 2, 2};
  if (!Check(surface->lock(&buffer, &dirty) == 0, "lock resized buffer")) return;
  Check(RectEquals(dirty, 0, 0, 9, 11), "resize invalidates full frame after preallocation");
  Check(surface->unlockAndPost() == 0, "publish resized frame");
}
}
