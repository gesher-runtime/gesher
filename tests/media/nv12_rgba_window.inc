// Isolated ANativeWindow adapter. Every call uses the production JNI dispatcher;
// the backing buffer, failure codes and native-reference counters are explicit.
struct ANativeWindow {
  int width, height, buffer_width, buffer_height, stride;
  int geometry_error = 0, lock_error = 0, post_error = 0;
  int references = 0, geometries = 0, locks = 0, posts = 0;
  size_t guard = 17;
  std::vector<uint8_t> storage;
  ANativeWindow(int w, int h, int padding)
      : width(w), height(h), buffer_width(w), buffer_height(h), stride(w + padding),
        storage(guard + static_cast<size_t>(stride) * h * 4 + guard, 0xcd) {}
};
struct ANativeWindow_Buffer { int width, height, stride; void* bits; };
ANativeWindow* nv12_window = nullptr;
jobject nv12_window_surface = nullptr;
ANativeWindow* ANativeWindow_fromSurface(JNIEnv*, jobject surface) {
  if (!nv12_window || surface != nv12_window_surface) return nullptr;
  ++nv12_window->references;
  return nv12_window;
}
void ANativeWindow_release(ANativeWindow* window) { --window->references; }
int ANativeWindow_getWidth(ANativeWindow* window) { return window->width; }
int ANativeWindow_getHeight(ANativeWindow* window) { return window->height; }
int ANativeWindow_setBuffersGeometry(ANativeWindow* window, int w, int h, int format) {
  ++window->geometries;
  Check(w > 0 && h > 0 && format == 1, "NV12 window geometry remains RGBA");
  return window->geometry_error;
}
int ANativeWindow_lock(ANativeWindow* window, ANativeWindow_Buffer* buffer, void*) {
  ++window->locks;
  if (window->lock_error) return window->lock_error;
  *buffer = {window->buffer_width, window->buffer_height, window->stride,
             window->storage.data() + window->guard};
  return 0;
}
int ANativeWindow_unlockAndPost(ANativeWindow* window) { ++window->posts; return window->post_error; }

