// libsurfacegl.so, the native code of tests/apps/surfacegl (surfaces track, ADR 0056):
// EGL/GLES 2 into a SurfaceView's surface, the way SDL and games draw.
//
// surfacegl_start(surface) takes the ANativeWindow (ANativeWindow_fromSurface), sets its
// buffers' format from the config's EGL_NATIVE_VISUAL_ID, makes an EGL window surface and
// an ES 2 context on a thread of its own, and draws frames: the left half red, the right
// half green (a scissored clear each), swapped with eglSwapBuffers, logging what EGL
// reports (surface size, frames) on stdout. surfacegl_stop ends the thread and destroys
// the surface and context (surfaceDestroyed).

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <jni.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pthread_t g_thread;
static volatile int g_running;
static ANativeWindow* g_window;

__attribute__((format(printf, 1, 2))) static void say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("surfacegl: ");
  vprintf(fmt, ap);
  printf("\n");
  fflush(stdout);
  va_end(ap);
}

static void* loop(void* arg) {
  (void)arg;
  EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major = 0, minor = 0;
  if (!eglInitialize(d, &major, &minor)) {
    say("eglInitialize failed 0x%x", eglGetError());
    return NULL;
  }
  const EGLint attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE,
                            EGL_WINDOW_BIT,      EGL_RED_SIZE,        8,
                            EGL_GREEN_SIZE,      8,                   EGL_BLUE_SIZE,
                            8,                   EGL_NONE};
  EGLConfig config;
  EGLint n = 0;
  if (!eglChooseConfig(d, attribs, &config, 1, &n) || n < 1) {
    say("eglChooseConfig found %d configs (0x%x)", n, eglGetError());
    return NULL;
  }
  EGLint visual = 0;
  eglGetConfigAttrib(d, config, EGL_NATIVE_VISUAL_ID, &visual);
  int err = ANativeWindow_setBuffersGeometry(g_window, 0, 0, visual);
  say("window %dx%d, setBuffersGeometry(visual %d)", ANativeWindow_getWidth(g_window),
      ANativeWindow_getHeight(g_window), visual);
  (void)err;
  EGLSurface s = eglCreateWindowSurface(d, config, g_window, NULL);
  if (s == EGL_NO_SURFACE) {
    say("eglCreateWindowSurface failed 0x%x", eglGetError());
    return NULL;
  }
  const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext c = eglCreateContext(d, config, EGL_NO_CONTEXT, ctx_attribs);
  if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, s, s, c)) {
    say("context failed 0x%x", eglGetError());
    return NULL;
  }
  int frames = 0, last_w = -1, last_h = -1;
  while (g_running) {
    EGLint w = 0, h = 0;
    eglQuerySurface(d, s, EGL_WIDTH, &w);
    eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w != last_w || h != last_h) {
      say("EGL surface %dx%d after %d frames", w, h, frames);
      last_w = w;
      last_h = h;
    }
    glViewport(0, 0, w, h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, w / 2, h);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(w / 2, 0, w - w / 2, h);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    if (!eglSwapBuffers(d, s)) {
      say("eglSwapBuffers failed 0x%x after %d frames", eglGetError(), frames);
      break;
    }
    if (++frames == 30) say("%d frames swapped (GL error 0x%x)", frames, glGetError());
  }
  eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(d, c);
  eglDestroySurface(d, s);
  say("stopped after %d frames", frames);
  return NULL;
}

JNIEXPORT void JNICALL Java_com_gesher_surfacegl_GlActivity_start(JNIEnv* env, jclass clazz,
                                                                  jobject surface) {
  (void)clazz;
  if (g_running) return;
  g_window = ANativeWindow_fromSurface(env, surface);
  if (g_window == NULL) {
    say("ANativeWindow_fromSurface answered null");
    return;
  }
  g_running = 1;
  pthread_create(&g_thread, NULL, loop, NULL);
}

JNIEXPORT void JNICALL Java_com_gesher_surfacegl_GlActivity_stop(JNIEnv* env, jclass clazz) {
  (void)env;
  (void)clazz;
  if (!g_running) return;
  g_running = 0;
  pthread_join(g_thread, NULL);
  ANativeWindow_release(g_window);
  g_window = NULL;
}
