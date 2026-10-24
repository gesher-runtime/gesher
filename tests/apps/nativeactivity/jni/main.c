// libnativeactivity.so, all the code of tests/apps/nativeactivity: an app with
// no Java code whose Activity is android.app.NativeActivity, written with the
// NDK's android_native_app_glue (built in from the NDK's sources).
//
//  - gesher_nativeactivity_onCreate (the manifest's android.app.func_name)
//    checks the ANativeActivity on the main thread (sdkVersion, data paths,
//    assetManager, JNI through activity->env on activity->clazz), calls the
//    glue's ANativeActivity_onCreate and chains the callbacks the glue leaves
//    out (onNativeWindowRedrawNeeded/Resized, onContentRectChanged) to log
//    them.
//  - android_main (the glue's thread) logs every lifecycle command; reads an
//    asset; calls Java from its own thread (AttachCurrentThread).
//  - The window (APP_CMD_INIT_WINDOW): EGL on the default display, a GLES 2
//    context on a window surface, a blue clear and a green triangle checked
//    with glReadPixels, eglSwapBuffers; then AChoreographer frame callbacks
//    (postFrameCallback64) redraw it every frame, as a game does (the first 30
//    frame times are checked), with a vsync callback (postVsyncCallback) and
//    the refresh rate callback.
//  - Input (AInputQueue through the glue): pointer down/up and keys are
//    logged. A click (pointer up) ends the GL rendering (the EGL surface is
//    destroyed, which on Android releases the window for the CPU) and draws a
//    CPU frame with ANativeWindow_lock: red, with a white 40x40 square where
//    the click was. Escape calls ANativeActivity_finish.
//
// Everything is logged with __android_log_print, tag NativeActivityApp
// (tests/windows/tracks/40-native-libs.ps1 checks the lines).

#include <android/asset_manager.h>
#include <android/choreographer.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <jni.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "android_native_app_glue.h"

#define TAG "NativeActivityApp"
#define LOG(...) ((void)__android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__))

struct engine {
  struct android_app* app;
  EGLDisplay display;
  EGLSurface surface;
  EGLContext context;
  int32_t width, height;
  GLuint program;
  GLint pos_attr, color_uniform;
  int gl;  // the window has the EGL surface
  int frames;
  int64_t first_frame_time, last_frame_time;
  int frame_times_increasing;
  int choreographer_running;
  int commands;
};

static const char* command_name(int32_t cmd) {
  static const char* const kNames[] = {
      "INPUT_CHANGED", "INIT_WINDOW", "TERM_WINDOW", "WINDOW_RESIZED", "WINDOW_REDRAW_NEEDED",
      "CONTENT_RECT_CHANGED", "GAINED_FOCUS", "LOST_FOCUS", "CONFIG_CHANGED", "LOW_MEMORY",
      "START", "RESUME", "SAVE_STATE", "PAUSE", "STOP", "DESTROY"};
  if (cmd >= 0 && cmd < (int32_t)(sizeof(kNames) / sizeof(kNames[0]))) return kNames[cmd];
  return "(other)";
}

// Java's Context.getPackageName() through JNI, from `env` (any thread's).
static void package_name(JNIEnv* env, jobject activity, char* out, size_t n) {
  snprintf(out, n, "(none)");
  jclass cls = (*env)->GetObjectClass(env, activity);
  jmethodID mid = (*env)->GetMethodID(env, cls, "getPackageName", "()Ljava/lang/String;");
  jstring s = mid != NULL ? (jstring)(*env)->CallObjectMethod(env, activity, mid) : NULL;
  if (s != NULL) {
    const char* chars = (*env)->GetStringUTFChars(env, s, NULL);
    snprintf(out, n, "%s", chars);
    (*env)->ReleaseStringUTFChars(env, s, chars);
    (*env)->DeleteLocalRef(env, s);
  }
  (*env)->DeleteLocalRef(env, cls);
}

// --- main-thread callbacks the glue does not use: logged, then chained -----------------------

static void (*glue_redraw_needed)(ANativeActivity*, ANativeWindow*);
static void (*glue_resized)(ANativeActivity*, ANativeWindow*);
static void (*glue_content_rect)(ANativeActivity*, const ARect*);

static void on_redraw_needed(ANativeActivity* activity, ANativeWindow* window) {
  LOG("main thread: onNativeWindowRedrawNeeded (%dx%d)", ANativeWindow_getWidth(window),
      ANativeWindow_getHeight(window));
  if (glue_redraw_needed != NULL) glue_redraw_needed(activity, window);
}
static void on_resized(ANativeActivity* activity, ANativeWindow* window) {
  LOG("main thread: onNativeWindowResized (%dx%d)", ANativeWindow_getWidth(window),
      ANativeWindow_getHeight(window));
  if (glue_resized != NULL) glue_resized(activity, window);
}
static void on_content_rect(ANativeActivity* activity, const ARect* r) {
  LOG("main thread: onContentRectChanged (%d, %d, %d, %d)", r->left, r->top, r->right, r->bottom);
  if (glue_content_rect != NULL) glue_content_rect(activity, r);
}

JNIEXPORT void gesher_nativeactivity_onCreate(ANativeActivity* activity, void* saved,
                                              size_t saved_size) {
  char pkg[128];
  package_name(activity->env, activity->clazz, pkg, sizeof(pkg));
  LOG("onCreate (func_name): sdk %d, internal %s, external %s, obb %s, assets %s, callbacks %s, "
      "package %s through activity->env",
      activity->sdkVersion, activity->internalDataPath != NULL ? activity->internalDataPath : "-",
      activity->externalDataPath != NULL ? activity->externalDataPath : "-",
      activity->obbPath != NULL ? activity->obbPath : "-",
      activity->assetManager != NULL ? "yes" : "no", activity->callbacks != NULL ? "yes" : "no",
      pkg);
  ANativeActivity_onCreate(activity, saved, saved_size);
  glue_redraw_needed = activity->callbacks->onNativeWindowRedrawNeeded;
  glue_resized = activity->callbacks->onNativeWindowResized;
  glue_content_rect = activity->callbacks->onContentRectChanged;
  activity->callbacks->onNativeWindowRedrawNeeded = on_redraw_needed;
  activity->callbacks->onNativeWindowResized = on_resized;
  activity->callbacks->onContentRectChanged = on_content_rect;
}

// --- EGL and GLES 2 ------------------------------------------------------------------------------

static GLuint shader(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512] = "";
    glGetShaderInfoLog(s, sizeof(log), NULL, log);
    LOG("gl: shader does not compile: %s", log);
  }
  return s;
}

static int gl_init(struct engine* e) {
  ANativeWindow* window = e->app->window;
  e->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major = 0, minor = 0;
  if (e->display == EGL_NO_DISPLAY || !eglInitialize(e->display, &major, &minor)) {
    LOG("gl: no EGL display (0x%x)", eglGetError());
    return 0;
  }
  const EGLint attribs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                            EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                            EGL_BLUE_SIZE, 8, EGL_NONE};
  // As the NDK's samples choose: the first config with exactly 8 bits of red,
  // green and blue (eglChooseConfig puts deeper ones first).
  EGLConfig configs[64];
  EGLint n = 0;
  if (!eglChooseConfig(e->display, attribs, configs, 64, &n) || n < 1) {
    LOG("gl: no EGL config (0x%x)", eglGetError());
    return 0;
  }
  EGLConfig config = configs[0];
  for (EGLint i = 0; i < n; i++) {
    EGLint r = 0, g = 0, b = 0;
    eglGetConfigAttrib(e->display, configs[i], EGL_RED_SIZE, &r);
    eglGetConfigAttrib(e->display, configs[i], EGL_GREEN_SIZE, &g);
    eglGetConfigAttrib(e->display, configs[i], EGL_BLUE_SIZE, &b);
    if (r == 8 && g == 8 && b == 8) {
      config = configs[i];
      break;
    }
  }
  EGLint format = 0;
  eglGetConfigAttrib(e->display, config, EGL_NATIVE_VISUAL_ID, &format);
  ANativeWindow_setBuffersGeometry(window, 0, 0, format);
  e->surface = eglCreateWindowSurface(e->display, config, window, NULL);
  const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  e->context = eglCreateContext(e->display, config, EGL_NO_CONTEXT, ctx_attribs);
  if (e->surface == EGL_NO_SURFACE || e->context == EGL_NO_CONTEXT ||
      !eglMakeCurrent(e->display, e->surface, e->surface, e->context)) {
    LOG("gl: no window surface or context (0x%x)", eglGetError());
    return 0;
  }
  eglQuerySurface(e->display, e->surface, EGL_WIDTH, &e->width);
  eglQuerySurface(e->display, e->surface, EGL_HEIGHT, &e->height);
  GLuint vs = shader(GL_VERTEX_SHADER,
                     "attribute vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n");
  GLuint fs = shader(GL_FRAGMENT_SHADER,
                     "precision mediump float;\nuniform vec4 color;\n"
                     "void main() { gl_FragColor = color; }\n");
  e->program = glCreateProgram();
  glAttachShader(e->program, vs);
  glAttachShader(e->program, fs);
  glLinkProgram(e->program);
  e->pos_attr = glGetAttribLocation(e->program, "pos");
  e->color_uniform = glGetUniformLocation(e->program, "color");
  LOG("gl: EGL %d.%d, %s, window surface %dx%d (visual id %d)", major, minor,
      (const char*)glGetString(GL_VERSION), e->width, e->height, format);
  e->gl = 1;
  return 1;
}

static void gl_term(struct engine* e) {
  if (e->display != EGL_NO_DISPLAY) {
    eglMakeCurrent(e->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (e->context != EGL_NO_CONTEXT) eglDestroyContext(e->display, e->context);
    if (e->surface != EGL_NO_SURFACE) eglDestroySurface(e->display, e->surface);
    eglTerminate(e->display);
  }
  e->display = EGL_NO_DISPLAY;
  e->surface = EGL_NO_SURFACE;
  e->context = EGL_NO_CONTEXT;
  e->gl = 0;
}

// A blue clear and a green triangle over the middle; with `check`, read back.
static void gl_draw(struct engine* e, int check) {
  static const GLfloat kTriangle[] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f};
  glViewport(0, 0, e->width, e->height);
  glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(e->program);
  glUniform4f(e->color_uniform, 0.0f, 1.0f, 0.0f, 1.0f);
  glVertexAttribPointer((GLuint)e->pos_attr, 2, GL_FLOAT, GL_FALSE, 0, kTriangle);
  glEnableVertexAttribArray((GLuint)e->pos_attr);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  if (check) {
    size_t count = (size_t)e->width * (size_t)e->height;
    unsigned char* px = malloc(count * 4);
    glReadPixels(0, 0, e->width, e->height, GL_RGBA, GL_UNSIGNED_BYTE, px);
    size_t green = 0, blue = 0, other = 0;
    for (size_t i = 0; i < count; i++) {
      const unsigned char* p = px + i * 4;
      if (p[0] < 16 && p[1] > 239 && p[2] < 16) green++;
      else if (p[0] < 16 && p[1] < 16 && p[2] > 239) blue++;
      else other++;
    }
    free(px);
    size_t want = count / 8;  // half the width by half the height, halved
    int ok = glGetError() == GL_NO_ERROR && other == 0 && green > want * 9 / 10 &&
             green < want * 11 / 10;
    LOG("gl frame: %dx%d, %zu green, %zu blue, %zu other: readpixels=%s", e->width, e->height,
        green, blue, other, ok ? "ok" : "FAIL");
  }
  EGLBoolean swapped = eglSwapBuffers(e->display, e->surface);
  if (check) LOG("gl frame: swap=%s", swapped ? "ok" : "FAIL");
}

// --- AChoreographer --------------------------------------------------------------------------------

// Frames from the choreographer, as a game renders: until the click ends the GL
// rendering. The first 30 frame times are checked and reported.
static void on_frame(int64_t frame_time, void* data) {
  struct engine* e = data;
  if (!e->gl || !e->choreographer_running) return;
  if (e->frames < 30) {
    if (e->frames == 0) e->first_frame_time = frame_time;
    else if (frame_time <= e->last_frame_time) e->frame_times_increasing = 0;
    e->last_frame_time = frame_time;
  }
  e->frames++;
  gl_draw(e, 0);
  if (e->frames == 30) {
    LOG("choreographer: %d frames drawn from postFrameCallback64, frame times %s, %.1f ms apart "
        "on average",
        e->frames, e->frame_times_increasing ? "increasing" : "NOT increasing",
        (double)(e->last_frame_time - e->first_frame_time) / 1e6 / (e->frames - 1));
  }
  AChoreographer_postFrameCallback64(AChoreographer_getInstance(), on_frame, e);
}

static void on_vsync(const AChoreographerFrameCallbackData* d, void* data) {
  (void)data;
  size_t n = AChoreographerFrameCallbackData_getFrameTimelinesLength(d);
  size_t i = AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(d);
  int64_t frame = AChoreographerFrameCallbackData_getFrameTimeNanos(d);
  int64_t present = AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(d, i);
  int64_t deadline = AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(d, i);
  LOG("choreographer: vsync callback, %zu frame timeline(s), presentation %s the frame time, "
      "deadline %s",
      n, present > frame ? "after" : "NOT after", deadline > frame ? "after it" : "NOT after it");
}

static void on_refresh_rate(int64_t period, void* data) {
  (void)data;
  LOG("choreographer: refresh rate callback, vsync period %lld ns", (long long)period);
}

// --- ANativeWindow_lock --------------------------------------------------------------------------

// Red, with a white 40x40 square centered on (x, y).
static void cpu_frame(struct engine* e, float x, float y) {
  ANativeWindow* window = e->app->window;
  if (window == NULL) return;
  int err = ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
  ANativeWindow_Buffer buffer;
  int lock = ANativeWindow_lock(window, &buffer, NULL);
  if (err != 0 || lock != 0) {
    LOG("cpu frame: setBuffersGeometry %d, lock %d: FAIL", err, lock);
    return;
  }
  int cx = (int)x, cy = (int)y;
  for (int32_t row = 0; row < buffer.height; row++) {
    uint32_t* line = (uint32_t*)buffer.bits + (size_t)row * (size_t)buffer.stride;
    for (int32_t col = 0; col < buffer.width; col++) {
      int white = col >= cx - 20 && col < cx + 20 && row >= cy - 20 && row < cy + 20;
      line[col] = white ? 0xffffffffu : 0xff0000ffu;  // RGBA bytes: white, red
    }
  }
  int post = ANativeWindow_unlockAndPost(window);
  LOG("cpu frame: %dx%d stride %d format %d, white square at (%d, %d): lock=ok, post=%s",
      buffer.width, buffer.height, buffer.stride, buffer.format, cx, cy, post == 0 ? "ok" : "FAIL");
}

// --- input -----------------------------------------------------------------------------------------

static int32_t on_input(struct android_app* app, AInputEvent* event) {
  struct engine* e = app->userData;
  int32_t type = AInputEvent_getType(event);
  if (type == AINPUT_EVENT_TYPE_MOTION) {
    int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_UP) {
      float x = AMotionEvent_getX(event, 0), y = AMotionEvent_getY(event, 0);
      LOG("input: motion %s at (%.0f, %.0f), source 0x%x, %zu pointer(s), tool %d",
          action == AMOTION_EVENT_ACTION_DOWN ? "down" : "up", x, y, AInputEvent_getSource(event),
          AMotionEvent_getPointerCount(event), AMotionEvent_getToolType(event, 0));
      if (action == AMOTION_EVENT_ACTION_UP) {
        // The click: GL lets the window go, the CPU draws it.
        if (e->gl) gl_term(e);
        cpu_frame(e, x, y);
      }
    }
    return 1;
  }
  if (type == AINPUT_EVENT_TYPE_KEY) {
    int32_t action = AKeyEvent_getAction(event);
    int32_t code = AKeyEvent_getKeyCode(event);
    LOG("input: key %s, keycode %d, scancode %d, meta 0x%x, repeat %d, source 0x%x",
        action == AKEY_EVENT_ACTION_DOWN ? "down" : action == AKEY_EVENT_ACTION_UP ? "up" : "other",
        code, AKeyEvent_getScanCode(event), AKeyEvent_getMetaState(event),
        AKeyEvent_getRepeatCount(event), AInputEvent_getSource(event));
    if (code == AKEYCODE_ESCAPE && action == AKEY_EVENT_ACTION_UP) {
      LOG("input: escape: ANativeActivity_finish");
      ANativeActivity_finish(app->activity);
    }
    return 1;
  }
  return 0;
}

// --- lifecycle -------------------------------------------------------------------------------------

static void on_command(struct android_app* app, int32_t cmd) {
  struct engine* e = app->userData;
  e->commands++;
  LOG("lifecycle: %s", command_name(cmd));
  switch (cmd) {
    case APP_CMD_INIT_WINDOW:
      if (app->window == NULL) break;
      LOG("window: %dx%d, format %d", ANativeWindow_getWidth(app->window),
          ANativeWindow_getHeight(app->window), ANativeWindow_getFormat(app->window));
      if (gl_init(e)) {
        gl_draw(e, 1);
        AChoreographer* choreographer = AChoreographer_getInstance();
        if (choreographer == NULL) {
          LOG("choreographer: none on this thread: FAIL");
          break;
        }
        e->frames = 0;
        e->frame_times_increasing = 1;
        e->choreographer_running = 1;
        AChoreographer_registerRefreshRateCallback(choreographer, on_refresh_rate, e);
        AChoreographer_postVsyncCallback(choreographer, on_vsync, e);
        AChoreographer_postFrameCallback64(choreographer, on_frame, e);
      } else {
        gl_term(e);
        cpu_frame(e, ANativeWindow_getWidth(app->window) / 2.0f,
                  ANativeWindow_getHeight(app->window) / 2.0f);
      }
      break;
    case APP_CMD_TERM_WINDOW:
      if (e->gl) gl_term(e);
      break;
    default:
      break;
  }
}

void android_main(struct android_app* app) {
  struct engine e;
  memset(&e, 0, sizeof(e));
  e.app = app;
  e.display = EGL_NO_DISPLAY;
  e.surface = EGL_NO_SURFACE;
  e.context = EGL_NO_CONTEXT;
  app->userData = &e;
  app->onAppCmd = on_command;
  app->onInputEvent = on_input;

  // An asset, through the ANativeActivity's AAssetManager.
  AAsset* asset = AAssetManager_open(app->activity->assetManager, "hello.txt", AASSET_MODE_BUFFER);
  char text[64] = "(none)";
  if (asset != NULL) {
    int n = AAsset_read(asset, text, sizeof(text) - 1);
    text[n > 0 ? n : 0] = 0;
    char* nl = strchr(text, '\n');
    if (nl != NULL) *nl = 0;
    AAsset_close(asset);
  }
  // Java from this thread.
  JNIEnv* env = NULL;
  char pkg[128] = "(none)";
  if ((*app->activity->vm)->AttachCurrentThread(app->activity->vm, &env, NULL) == JNI_OK) {
    package_name(env, app->activity->clazz, pkg, sizeof(pkg));
    (*app->activity->vm)->DetachCurrentThread(app->activity->vm);
  }
  LOG("android_main: asset \"%s\", package %s through AttachCurrentThread", text, pkg);

  while (!app->destroyRequested) {
    struct android_poll_source* source = NULL;
    int result = ALooper_pollOnce(-1, NULL, NULL, (void**)&source);
    if (result == ALOOPER_POLL_ERROR) {
      LOG("android_main: ALooper_pollOnce failed");
      break;
    }
    if (source != NULL) source->process(app, source);
  }
  if (e.gl) gl_term(&e);
  LOG("android_main: done after %d lifecycle commands", e.commands);
}
