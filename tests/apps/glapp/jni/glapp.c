// libglapp.so, the native code of tests/apps/glapp: drawing into a window
// from an NDK library, the two ways NDK code does it.
//
//  - glapp_cpu_draw: ANativeWindow_setBuffersGeometry, ANativeWindow_lock,
//    four colored quadrants written into the buffer (RGBA_8888, by stride),
//    ANativeWindow_unlockAndPost.
//  - glapp_gl_draw: EGL on the default display (eglChooseConfig with the
//    Android-only EGL_RECORDABLE_ANDROID, EGL_NATIVE_VISUAL_ID passed to
//    ANativeWindow_setBuffersGeometry, as NDK samples do),
//    eglCreateWindowSurface on the ANativeWindow, an OpenGL ES 2 or 3 context,
//    a blue clear and a green triangle (GLSL ES 1.00 or 3.00, a vertex buffer,
//    and a vertex array object on ES 3), read back with glReadPixels and
//    checked, then eglSwapBuffers. glapp_gl_finish destroys the surface and
//    context.
//
// Each reports in `out` and returns 0 when every step worked. Used by
// gesher-app through GlActivity's natives (the window from a Java Surface,
// ANativeWindow_fromSurface), and directly by tests/native/gl_window_test.cc
// on a window made with gesher_window_create.

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <jni.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

static void append(char* out, size_t n, const char* fmt, ...) {
  size_t len = strlen(out);
  if (len >= n) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(out + len, n - len, fmt, ap);
  va_end(ap);
}

// --- ANativeWindow_lock ---------------------------------------------------------------------------

// Quadrants: top left red, top right green, bottom left blue, bottom right white.
static void quadrant_color(int x, int y, int w, int h, unsigned char rgba[4]) {
  int right = x >= w / 2, bottom = y >= h / 2;
  static const unsigned char kColors[4][4] = {
      {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}};
  memcpy(rgba, kColors[bottom * 2 + right], 4);
}

EXPORT int glapp_cpu_draw(ANativeWindow* window, char* out, size_t n) {
  out[0] = 0;
  int32_t w = ANativeWindow_getWidth(window), h = ANativeWindow_getHeight(window);
  int32_t format = ANativeWindow_getFormat(window);
  append(out, n, "window %dx%d format=%d", w, h, format);
  if (w <= 0 || h <= 0) return append(out, n, " FAIL: no size"), 1;
  int err = ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
  if (err != 0) return append(out, n, " FAIL: setBuffersGeometry=%d", err), 1;
  ANativeWindow_Buffer buffer;
  err = ANativeWindow_lock(window, &buffer, NULL);
  if (err != 0) return append(out, n, " FAIL: lock=%d", err), 1;
  append(out, n, ", locked %dx%d stride=%d format=%d", buffer.width, buffer.height, buffer.stride,
         buffer.format);
  if (buffer.format != WINDOW_FORMAT_RGBA_8888 || buffer.stride < buffer.width) {
    ANativeWindow_unlockAndPost(window);
    return append(out, n, " FAIL: unexpected buffer"), 1;
  }
  for (int y = 0; y < buffer.height; y++) {
    unsigned char* row = (unsigned char*)buffer.bits + (size_t)y * buffer.stride * 4;
    for (int x = 0; x < buffer.width; x++) {
      quadrant_color(x, y, buffer.width, buffer.height, row + x * 4);
    }
  }
  err = ANativeWindow_unlockAndPost(window);
  if (err != 0) return append(out, n, " FAIL: unlockAndPost=%d", err), 1;
  append(out, n, ", posted: lock ok");
  return 0;
}

// --- EGL and GLES ---------------------------------------------------------------------------------

static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLSurface g_surface = EGL_NO_SURFACE;
static EGLContext g_context = EGL_NO_CONTEXT;

static GLuint compile(GLenum type, const char* source, char* out, size_t n) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[256] = "";
    glGetShaderInfoLog(shader, sizeof log, NULL, log);
    append(out, n, " FAIL: shader: %s", log);
    return 0;
  }
  return shader;
}

EXPORT void glapp_gl_finish(void) {
  if (g_display == EGL_NO_DISPLAY) return;
  eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (g_surface != EGL_NO_SURFACE) eglDestroySurface(g_display, g_surface);
  if (g_context != EGL_NO_CONTEXT) eglDestroyContext(g_display, g_context);
  g_surface = EGL_NO_SURFACE;
  g_context = EGL_NO_CONTEXT;
}

#define FAIL(...) return append(out, n, " FAIL: " __VA_ARGS__), 1

// GLES 3, as Flutter's Impeller draws: arguments the System V -> Microsoft
// thunks must pass whole. GLintptr/GLsizeiptr are 64 bits here and in ANGLE
// (khrplatform.h spells them `long`, which is 32 bits where the thunks are
// compiled): glBindBufferRange's size is its fifth argument, on the Microsoft
// stack, where a 32-bit store leaves the slot's upper half as it was; Impeller
// then bound every uniform block with a huge size and drew nothing but its
// clear colour. Checked: the range ANGLE records for a size above 4 GiB
// (glGetInteger64i_v), then a draw into a framebuffer whose colour comes from a
// uniform block bound at an aligned offset (glBindBufferRange), a 9-argument
// glTexImage2D, a 10-argument glBlitFramebuffer and a 7-argument glReadPixels.
static int check_buffer_ranges(char* out, size_t n) {
  GLint align = 0;
  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
  if (align <= 0 || align > 4096) FAIL("GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT=%d", align);
  GLuint ubo = 0;
  glGenBuffers(1, &ubo);
  glBindBuffer(GL_UNIFORM_BUFFER, ubo);
  unsigned char data[8192];
  memset(data, 0, sizeof data);
  const GLfloat kColor[4] = {1.0f, 0.0f, 1.0f, 1.0f};  // magenta, at offset `align`
  memcpy(data + align, kColor, sizeof kColor);
  glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)(align + 4096), data, GL_STATIC_DRAW);

  const GLsizeiptr kHuge = (GLsizeiptr)0x100000010LL;  // 4 GiB + 16: needs the upper half
  glBindBufferRange(GL_UNIFORM_BUFFER, 0, ubo, (GLintptr)align, kHuge);
  GLint64 start = -1, size = -1;
  glGetInteger64i_v(GL_UNIFORM_BUFFER_START, 0, &start);
  glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, 0, &size);
  if (start != align || size != kHuge) {
    FAIL("glBindBufferRange(offset %d, size 0x%llx) recorded offset %lld, size 0x%llx", align,
         (unsigned long long)kHuge, (long long)start, (unsigned long long)size);
  }
  glBindBufferRange(GL_UNIFORM_BUFFER, 0, ubo, (GLintptr)align, (GLsizeiptr)(4 * sizeof(GLfloat)));
  glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, 0, &size);
  if (size != (GLint64)(4 * sizeof(GLfloat))) FAIL("uniform buffer size %lld", (long long)size);

  const char* vs = "#version 300 es\nin vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
  const char* fs = "#version 300 es\nprecision mediump float;\n"
                   "layout(std140) uniform FragInfo { vec4 color; } frag_info;\nout vec4 color;\n"
                   "void main() { color = frag_info.color; }\n";
  GLuint v = compile(GL_VERTEX_SHADER, vs, out, n);
  GLuint f = compile(GL_FRAGMENT_SHADER, fs, out, n);
  if (v == 0 || f == 0) return 1;
  GLuint program = glCreateProgram();
  glAttachShader(program, v);
  glAttachShader(program, f);
  glBindAttribLocation(program, 0, "pos");
  glLinkProgram(program);
  GLint linked = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) FAIL("uniform block program link");
  GLuint block = glGetUniformBlockIndex(program, "FragInfo");
  if (block == GL_INVALID_INDEX) FAIL("no FragInfo block");
  glUniformBlockBinding(program, block, 0);
  glUseProgram(program);

  // A 4x4 texture to draw into, and one to blit to.
  enum { kSide = 4 };
  GLuint tex[2] = {0, 0}, fbo[2] = {0, 0};
  glGenTextures(2, tex);
  glGenFramebuffers(2, fbo);
  for (int i = 0; i < 2; i++) {
    glBindTexture(GL_TEXTURE_2D, tex[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kSide, kSide, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[i], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) FAIL("framebuffer %d", i);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
  glViewport(0, 0, kSide, kSide);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  static const GLfloat kQuad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
  GLuint vao = 0, vbo = 0;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  // The quad at offset 64 of a larger buffer, as Impeller's host buffer has it.
  GLfloat vertices[16 + 8];
  memset(vertices, 0, sizeof vertices);
  memcpy(vertices + 16, kQuad, sizeof kQuad);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof vertices, NULL, GL_STREAM_DRAW);
  glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)0, (GLsizeiptr)sizeof vertices, vertices);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), (const void*)(16 * sizeof(GLfloat)));
  glEnableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[0]);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[1]);
  glBlitFramebuffer(0, 0, kSide, kSide, 0, 0, kSide, kSide, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
  unsigned char px[kSide * kSide * 4];
  memset(px, 0x55, sizeof px);
  glReadPixels(0, 0, kSide, kSide, GL_RGBA, GL_UNSIGNED_BYTE, px);
  GLenum gl_error = glGetError();

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(2, fbo);
  glDeleteTextures(2, tex);
  glDeleteBuffers(1, &vbo);
  glDeleteBuffers(1, &ubo);
  glDeleteVertexArrays(1, &vao);
  glDeleteProgram(program);
  if (gl_error != GL_NO_ERROR) FAIL("uniform block draw: GL error 0x%x", gl_error);
  for (int i = 0; i < kSide * kSide; i++) {
    const unsigned char* p = px + i * 4;
    if (p[0] != 255 || p[1] != 0 || p[2] != 255) {
      FAIL("uniform block draw: pixel %d is %02x%02x%02x, not the block's magenta", i, p[0], p[1],
           p[2]);
    }
  }
  append(out, n, ", buffer ranges=ok");
  return 0;
}

EXPORT int glapp_gl_draw(ANativeWindow* window, int es, char* out, size_t n) {
  out[0] = 0;
  g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (g_display == EGL_NO_DISPLAY) FAIL("eglGetDisplay");
  EGLint major = 0, minor = 0;
  if (!eglInitialize(g_display, &major, &minor)) FAIL("eglInitialize 0x%x", eglGetError());
  append(out, n, "EGL %d.%d", major, minor);

  const EGLint config_attribs[] = {
      EGL_RENDERABLE_TYPE, es >= 3 ? EGL_OPENGL_ES3_BIT_KHR : EGL_OPENGL_ES2_BIT,
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_RECORDABLE_ANDROID, EGL_TRUE,
      EGL_NONE};
  EGLConfig config;
  EGLint configs = 0;
  if (!eglChooseConfig(g_display, config_attribs, &config, 1, &configs) || configs < 1) {
    FAIL("eglChooseConfig 0x%x (%d configs)", eglGetError(), configs);
  }
  EGLint visual = 0;
  if (!eglGetConfigAttrib(g_display, config, EGL_NATIVE_VISUAL_ID, &visual)) {
    FAIL("EGL_NATIVE_VISUAL_ID 0x%x", eglGetError());
  }
  int err = ANativeWindow_setBuffersGeometry(window, 0, 0, visual);
  append(out, n, ", visual=%d geometry=%d", visual, err);
  if (err != 0) FAIL("setBuffersGeometry");

  g_surface = eglCreateWindowSurface(g_display, config, window, NULL);
  if (g_surface == EGL_NO_SURFACE) FAIL("eglCreateWindowSurface 0x%x", eglGetError());
  const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, es, EGL_NONE};
  g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, context_attribs);
  if (g_context == EGL_NO_CONTEXT) FAIL("eglCreateContext 0x%x", eglGetError());
  if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
    FAIL("eglMakeCurrent 0x%x", eglGetError());
  }
  EGLint w = 0, h = 0;
  eglQuerySurface(g_display, g_surface, EGL_WIDTH, &w);
  eglQuerySurface(g_display, g_surface, EGL_HEIGHT, &h);
  append(out, n, ", surface %dx%d, GL_VERSION=%s, GL_RENDERER=%s", w, h,
         (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
  if (w <= 0 || h <= 0) FAIL("surface size");

  // eglGetProcAddress gives callable (System V) entry points.
  PFNGLCLEARCOLORPROC clear_color = (PFNGLCLEARCOLORPROC)eglGetProcAddress("glClearColor");
  if (clear_color == NULL) FAIL("eglGetProcAddress(glClearColor)");

  glViewport(0, 0, w, h);
  clear_color(0.0f, 0.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  const char* vs = es >= 3 ? "#version 300 es\nin vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n"
                           : "attribute vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
  const char* fs = es >= 3 ? "#version 300 es\nprecision mediump float;\nout vec4 color;\n"
                             "void main() { color = vec4(0.0, 1.0, 0.0, 1.0); }\n"
                           : "precision mediump float;\n"
                             "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";
  GLuint v = compile(GL_VERTEX_SHADER, vs, out, n);
  GLuint f = compile(GL_FRAGMENT_SHADER, fs, out, n);
  if (v == 0 || f == 0) return 1;
  GLuint program = glCreateProgram();
  glAttachShader(program, v);
  glAttachShader(program, f);
  glBindAttribLocation(program, 0, "pos");
  glLinkProgram(program);
  GLint linked = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) FAIL("link");
  glUseProgram(program);

  static const GLfloat kTriangle[] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f};
  GLuint vao = 0, vbo = 0;
  if (es >= 3) {
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
  }
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof kTriangle, kTriangle, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
  glEnableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  unsigned char* pixels = malloc((size_t)w * h * 4);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  GLenum gl_error = glGetError();
  int green = 0, blue = 0, other = 0;
  for (int i = 0; i < w * h; i++) {
    const unsigned char* p = pixels + i * 4;
    if (p[0] == 0 && p[1] == 255 && p[2] == 0) {
      green++;
    } else if (p[0] == 0 && p[1] == 0 && p[2] == 255) {
      blue++;
    } else {
      other++;
    }
  }
#define AT(x, y) (pixels + ((size_t)(y) * w + (x)) * 4)
  int center_green = AT(w / 2, h / 2)[1] == 255 && AT(w / 2, h / 2)[2] == 0;
  int corners_blue = AT(0, 0)[2] == 255 && AT(w - 1, 0)[2] == 255 && AT(0, h - 1)[2] == 255 &&
                     AT(w - 1, h - 1)[2] == 255 && AT(0, 0)[1] == 0 && AT(w - 1, h - 1)[1] == 0;
#undef AT
  free(pixels);
  // The triangle covers 1/8 of the viewport.
  int expected = w * h / 8;
  int area_ok = green > expected * 8 / 10 && green < expected * 12 / 10;
  append(out, n, ", glReadPixels: %d green, %d blue, %d other", green, blue, other);
  if (gl_error != GL_NO_ERROR) FAIL("GL error 0x%x", gl_error);
  if (!center_green || !corners_blue || !area_ok || other > w * h / 50) FAIL("pixels");
  append(out, n, ": readpixels=ok");
  if (!eglSwapBuffers(g_display, g_surface)) FAIL("eglSwapBuffers 0x%x", eglGetError());
  append(out, n, ", swap=ok");
  if (es >= 3 && check_buffer_ranges(out, n) != 0) return 1;
  return 0;
}

// --- GlActivity's natives (gesher-app) ------------------------------------------------------------

JNIEXPORT jstring JNICALL Java_com_gesher_glapp_GlActivity_cpuDraw(JNIEnv* env, jclass clazz,
                                                                   jobject surface) {
  (void)clazz;
  char out[512] = "";
  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (window == NULL) return (*env)->NewStringUTF(env, "FAIL: ANativeWindow_fromSurface gave null");
  int rc = glapp_cpu_draw(window, out, sizeof out);
  ANativeWindow_release(window);
  char result[600];
  snprintf(result, sizeof result, "%s %s", rc == 0 ? "ok:" : "FAIL:", out);
  return (*env)->NewStringUTF(env, result);
}

JNIEXPORT jstring JNICALL Java_com_gesher_glapp_GlActivity_glDraw(JNIEnv* env, jclass clazz,
                                                                  jobject surface, jint es) {
  (void)clazz;
  char out[768] = "";
  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (window == NULL) return (*env)->NewStringUTF(env, "FAIL: ANativeWindow_fromSurface gave null");
  int rc = glapp_gl_draw(window, es, out, sizeof out);
  glapp_gl_finish();
  ANativeWindow_release(window);
  char result[800];
  snprintf(result, sizeof result, "%s %s", rc == 0 ? "ok:" : "FAIL:", out);
  return (*env)->NewStringUTF(env, result);
}
