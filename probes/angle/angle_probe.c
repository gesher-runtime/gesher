// ANGLE on Windows: an EGL display on Direct3D 11 (hardware, else WARP), a
// pbuffer surface, an OpenGL ES 3 context, shaders compiled and linked, a
// colored draw and a textured draw, and the pixels read back and checked.
// Built with llvm-mingw against ANGLE's import libraries (EGL and GLES are C
// ABIs); run in CI on Windows (tests/windows/tracks/31-angle.ps1) with
// libEGL.dll and libGLESv2.dll next to it. Prints "all passed" when every
// check holds.
//
//   angle_probe [--warp]     --warp: WARP only, without trying the hardware
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>

static int failures;

static void check(int ok, const char *what) {
  printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static void check_gl(const char *what) {
  GLenum e = glGetError();
  if (e != GL_NO_ERROR) {
    printf("FAIL  %s: GL error 0x%04x\n", what, e);
    failures++;
  }
}

static EGLDisplay open_display(EGLint device_type) {
  PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
  if (!get_platform_display) return EGL_NO_DISPLAY;
  const EGLint attribs[] = {
      EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
      EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, device_type,
      EGL_NONE,
  };
  EGLDisplay dpy = get_platform_display(EGL_PLATFORM_ANGLE_ANGLE, (void *)EGL_DEFAULT_DISPLAY, attribs);
  if (dpy == EGL_NO_DISPLAY) return EGL_NO_DISPLAY;
  EGLint major = 0, minor = 0;
  if (!eglInitialize(dpy, &major, &minor)) {
    printf("info  eglInitialize failed: 0x%04x\n", eglGetError());
    return EGL_NO_DISPLAY;
  }
  printf("info  EGL %d.%d, %s\n", major, minor, eglQueryString(dpy, EGL_VERSION));
  return dpy;
}

static GLuint compile(GLenum type, const char *source) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &source, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048] = {0};
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    printf("info  shader log: %s\n", log);
  }
  check(ok, type == GL_VERTEX_SHADER ? "GLSL ES 3.00 vertex shader compiles"
                                     : "GLSL ES 3.00 fragment shader compiles");
  return s;
}

static GLuint link(GLuint vs, GLuint fs, const char *what) {
  GLuint p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glBindAttribLocation(p, 0, "pos");
  glLinkProgram(p);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048] = {0};
    glGetProgramInfoLog(p, sizeof log, NULL, log);
    printf("info  program log: %s\n", log);
  }
  check(ok, what);
  return p;
}

enum { W = 64, H = 64 };

// RGBA at (x, y), y from the bottom as glReadPixels returns it.
static const unsigned char *px(const unsigned char *buf, int x, int y) {
  return buf + 4 * (y * W + x);
}

static int is(const unsigned char *p, int r, int g, int b) {
  return abs(p[0] - r) <= 2 && abs(p[1] - g) <= 2 && abs(p[2] - b) <= 2 && p[3] >= 253;
}

int main(int argc, char **argv) {
  int warp_only = argc > 1 && strcmp(argv[1], "--warp") == 0;
  setvbuf(stdout, NULL, _IONBF, 0);

  // 1. The display: Direct3D 11 on the hardware adapter, else WARP.
  EGLDisplay dpy = EGL_NO_DISPLAY;
  const char *device = "hardware";
  if (!warp_only) dpy = open_display(EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE);
  if (dpy == EGL_NO_DISPLAY) {
    device = "WARP";
    dpy = open_display(EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE);
  }
  if (dpy == EGL_NO_DISPLAY) {
    printf("FAIL  no EGL display on Direct3D 11 (hardware or WARP): 0x%04x\n", eglGetError());
    return 1;
  }
  printf("info  device: D3D11 %s\n", device);
  check(1, "EGL display on Direct3D 11");
  const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
  check(exts && strstr(exts, "EGL_KHR_create_context"), "EGL_KHR_create_context");

  // 2. A pbuffer surface with an ES 3 capable RGBA8 config.
  const EGLint config_attribs[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_NONE,
  };
  EGLConfig config;
  EGLint n = 0;
  if (!eglChooseConfig(dpy, config_attribs, &config, 1, &n) || n < 1) {
    printf("FAIL  no ES 3 pbuffer config: 0x%04x\n", eglGetError());
    return 1;
  }
  check(1, "RGBA8 ES 3 pbuffer config");
  const EGLint surface_attribs[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
  EGLSurface surface = eglCreatePbufferSurface(dpy, config, surface_attribs);
  if (surface == EGL_NO_SURFACE) {
    printf("FAIL  eglCreatePbufferSurface: 0x%04x\n", eglGetError());
    return 1;
  }
  check(1, "64x64 pbuffer surface");

  // 3. An OpenGL ES 3.0 context.
  if (!eglBindAPI(EGL_OPENGL_ES_API)) printf("info  eglBindAPI: 0x%04x\n", eglGetError());
  const EGLint context_attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, context_attribs);
  if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surface, surface, ctx)) {
    printf("FAIL  ES 3 context: 0x%04x\n", eglGetError());
    return 1;
  }
  const char *version = (const char *)glGetString(GL_VERSION);
  printf("info  GL_VENDOR   %s\n", glGetString(GL_VENDOR));
  printf("info  GL_RENDERER %s\n", glGetString(GL_RENDERER));
  printf("info  GL_VERSION  %s\n", version);
  printf("info  GLSL        %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
  check(version && strncmp(version, "OpenGL ES 3.", 12) == 0, "OpenGL ES 3 context current");
  GLint major = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  check(major >= 3, "GL_MAJOR_VERSION >= 3");

  // 4. Shaders: one flat color, one sampling a texture.
  GLuint vs = compile(GL_VERTEX_SHADER,
                      "#version 300 es\n"
                      "in vec2 pos;\n"
                      "out vec2 uv;\n"
                      "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n");
  GLuint fs_color = compile(GL_FRAGMENT_SHADER,
                            "#version 300 es\n"
                            "precision mediump float;\n"
                            "uniform vec4 color;\n"
                            "out vec4 frag;\n"
                            "void main() { frag = color; }\n");
  GLuint fs_tex = compile(GL_FRAGMENT_SHADER,
                          "#version 300 es\n"
                          "precision mediump float;\n"
                          "uniform sampler2D tex;\n"
                          "in vec2 uv;\n"
                          "out vec4 frag;\n"
                          "void main() { frag = texture(tex, uv); }\n");
  GLuint p_color = link(vs, fs_color, "color program links");
  GLuint p_tex = link(vs, fs_tex, "texture program links");
  check_gl("shaders");

  // A vertex array with two quads: the left half and the right half.
  static const GLfloat quads[] = {
      -1, -1, 0, -1, 0, 1, -1, -1, 0, 1, -1, 1,  // left half
      0, -1, 1, -1, 1, 1, 0, -1, 1, 1, 0, 1,     // right half
  };
  GLuint vao, vbo;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof quads, quads, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
  check_gl("vertex array");

  // A 2x2 texture: red, green / blue, white (rows from the bottom).
  static const unsigned char texels[] = {
      255, 0, 0, 255, 0, 255, 0, 255,
      0, 0, 255, 255, 255, 255, 255, 255,
  };
  GLuint tex;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  check_gl("texture upload");

  // 5. Draw: clear to gray, the left half yellow, the texture over the right
  // half (uv spans the whole surface, so the right half shows its right
  // column: green above white).
  glViewport(0, 0, W, H);
  glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(p_color);
  glUniform4f(glGetUniformLocation(p_color, "color"), 1.0f, 1.0f, 0.0f, 1.0f);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glUseProgram(p_tex);
  glUniform1i(glGetUniformLocation(p_tex, "tex"), 0);
  glDrawArrays(GL_TRIANGLES, 6, 6);
  check_gl("draw");

  // 6. Read back and check.
  static unsigned char buf[W * H * 4];
  glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  check_gl("glReadPixels");
  const unsigned char *a = px(buf, 16, 16), *b = px(buf, 48, 16), *c = px(buf, 48, 48);
  printf("info  (16,16) %d,%d,%d,%d  (48,16) %d,%d,%d,%d  (48,48) %d,%d,%d,%d\n",
         a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], c[0], c[1], c[2], c[3]);
  check(is(a, 255, 255, 0), "left half drawn yellow");
  check(is(b, 0, 255, 0), "texture: green texel at the lower right");
  check(is(c, 255, 255, 255), "texture: white texel at the upper right");
  int yellow = 0, textured = 0, other = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      const unsigned char *p = px(buf, x, y);
      if (x < W / 2 && is(p, 255, 255, 0)) yellow++;
      else if (x >= W / 2 && (is(p, 0, 255, 0) || is(p, 255, 255, 255))) textured++;
      else other++;
    }
  printf("info  %d yellow, %d textured, %d other pixels\n", yellow, textured, other);
  check(yellow == W * H / 2 && textured == W * H / 2, "every pixel as drawn");

  // 7. Clean up.
  glDeleteProgram(p_color);
  glDeleteProgram(p_tex);
  glDeleteShader(vs);
  glDeleteShader(fs_color);
  glDeleteShader(fs_tex);
  glDeleteTextures(1, &tex);
  glDeleteBuffers(1, &vbo);
  glDeleteVertexArrays(1, &vao);
  check_gl("delete");
  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(dpy, ctx);
  eglDestroySurface(dpy, surface);
  check(eglTerminate(dpy) == EGL_TRUE, "eglTerminate");

  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("all passed (D3D11 %s)\n", device);
  return 0;
}
