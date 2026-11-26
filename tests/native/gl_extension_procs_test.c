// The entry points of the GL extensions Gesher's GLES (ANGLE) advertises, from
// an x86-64 Android library through the bionic shim. EGL requires
// eglGetProcAddress to return the functions of every extension the context's
// GL_EXTENSIONS lists; a client takes them as present when it sees the
// extension (WebRender calls glProvokingVertexANGLE for
// GL_ANGLE_provoking_vertex, Skia's GrGLInterface requires
// glBindUniformLocationCHROMIUM). For each extension below that this context
// advertises, every function must resolve, through eglGetProcAddress and
// through dlsym on libGLESv2.so; glProvokingVertexANGLE is then called and its
// state read back, and glGetInteger64vEXT is called, through those pointers.
// Built by native-libs.yml as gl_extension_procs_test.so; gl_extension_test
// (compat/bionic/ndk_graphics.cmake) calls gesher_selftest().
#include <dlfcn.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(const char* what, int ok) {
  printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

struct Extension {
  const char* name;
  const char* functions[8];
};

// Extensions ANGLE adds beyond AOSP's headers (gl2ext_angle.h), and one whose
// later revision added an entry point AOSP's gl2ext.h lacks.
static const struct Extension kExtensions[] = {
    {"GL_ANGLE_provoking_vertex", {"glProvokingVertexANGLE"}},
    {"GL_ANGLE_copy_texture_3d", {"glCopyTexture3DANGLE", "glCopySubTexture3DANGLE"}},
    {"GL_ANGLE_polygon_mode", {"glPolygonModeANGLE"}},
    {"GL_ANGLE_request_extension", {"glRequestExtensionANGLE", "glDisableExtensionANGLE"}},
    {"GL_ANGLE_texture_multisample",
     {"glTexStorage2DMultisampleANGLE", "glGetMultisamplefvANGLE", "glSampleMaskiANGLE"}},
    {"GL_ANGLE_get_tex_level_parameter",
     {"glGetTexLevelParameterivANGLE", "glGetTexLevelParameterfvANGLE"}},
    {"GL_ANGLE_robust_client_memory",
     {"glGetIntegervRobustANGLE", "glReadPixelsRobustANGLE", "glTexImage2DRobustANGLE"}},
    {"GL_ANGLE_shader_pixel_local_storage",
     {"glBeginPixelLocalStorageANGLE", "glEndPixelLocalStorageANGLE"}},
    {"GL_ANGLE_multi_draw", {"glMultiDrawArraysANGLE", "glMultiDrawElementsANGLE"}},
    {"GL_ANGLE_logic_op", {"glLogicOpANGLE"}},
    {"GL_CHROMIUM_bind_uniform_location", {"glBindUniformLocationCHROMIUM"}},
    {"GL_CHROMIUM_copy_texture", {"glCopyTextureCHROMIUM", "glCopySubTextureCHROMIUM"}},
    {"GL_CHROMIUM_copy_compressed_texture", {"glCompressedCopyTextureCHROMIUM"}},
    {"GL_CHROMIUM_lose_context", {"glLoseContextCHROMIUM"}},
    {"GL_EXT_disjoint_timer_query", {"glQueryCounterEXT", "glGetInteger64vEXT"}},
};

static int has_extension(const char* list, const char* name) {
  size_t n = strlen(name);
  for (const char* p = list; (p = strstr(p, name)) != NULL; p += n) {
    if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return 1;
  }
  return 0;
}

// Test-only: ANGLE's own WARP display, uncapped (ES 3), through
// eglGetPlatformDisplayEXT's pass-through; never what an app gets by default.
static EGLDisplay angle_warp_display(void) {
  typedef EGLDisplay (*GetPlatformDisplayExt)(EGLenum, void*, const EGLint*);  // EGL_EXT_platform_base
  GetPlatformDisplayExt get = (GetPlatformDisplayExt)eglGetProcAddress("eglGetPlatformDisplayEXT");
  // D3D11, WARP, no feature level cap; a display key of its own (with the
  // device id ANGLE requires beside it, ignored for WARP): ANGLE keys displays
  // by platform type and key, not device type, so without one this would be
  // Android's default display itself.
  static const EGLint attribs[] = {0x3203, 0x3208, 0x3209, 0x320B, 0x34D7, 0x8c, 0x34DC, 0x7e57, EGL_NONE};
  return get != NULL ? get(0x3202, EGL_DEFAULT_DISPLAY, attribs) : EGL_NO_DISPLAY;
}

// The checks on `display`, with an ES 3 context where it has ES 3 configs, else
// ES 2. The ES version used, 0 when the context could not be made current.
static int check_display(EGLDisplay display, const char* which) {
  printf("  on %s\n", which);
  EGLint major = 0, minor = 0;
  check("eglInitialize", display != EGL_NO_DISPLAY && eglInitialize(display, &major, &minor));
  // ES 3 where the device has it; ES 2 on a software Direct3D device, which
  // gives no ES 3 (docs/system-features.md).
  EGLint es = 3;
  EGLint config_attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                             EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                             EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */, EGL_NONE};
  EGLConfig config = NULL;
  EGLint count = 0;
  if (!eglChooseConfig(display, config_attribs, &config, 1, &count) || count != 1) {
    es = 2;
    config_attribs[9] = EGL_OPENGL_ES2_BIT;
  }
  printf("  an ES %d context\n", es);
  check("eglChooseConfig (pbuffer)", eglChooseConfig(display, config_attribs, &config, 1, &count) && count == 1);
  static const EGLint kSurface[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
  EGLSurface surface = eglCreatePbufferSurface(display, config, kSurface);
  const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, es, EGL_NONE};
  EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
  check("eglMakeCurrent", surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT &&
                              eglMakeCurrent(display, surface, surface, context));
  if (failures != 0) return 0;
  const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
  check("GL_EXTENSIONS", extensions != NULL);
  if (extensions == NULL) return 0;

  void* gles = dlopen("libGLESv2.so", RTLD_NOW);
  check("dlopen(libGLESv2.so)", gles != NULL);
  int advertised = 0;
  for (size_t i = 0; i < sizeof(kExtensions) / sizeof(kExtensions[0]); ++i) {
    const struct Extension* e = &kExtensions[i];
    if (!has_extension(extensions, e->name)) {
      printf("  %-66s not advertised\n", e->name);
      continue;
    }
    ++advertised;
    for (int f = 0; f < 8 && e->functions[f] != NULL; ++f) {
      char what[160];
      snprintf(what, sizeof(what), "%s: %s", e->name, e->functions[f]);
      check(what, eglGetProcAddress(e->functions[f]) != NULL &&
                      (gles == NULL || dlsym(gles, e->functions[f]) != NULL));
    }
  }
  printf("  %d of %zu listed extensions advertised\n", advertised,
         sizeof(kExtensions) / sizeof(kExtensions[0]));

  // The thunks forward their arguments: set and read back the provoking vertex.
  if (has_extension(extensions, "GL_ANGLE_provoking_vertex")) {
    typedef void (*ProvokingVertex)(GLenum);
    ProvokingVertex provoking = (ProvokingVertex)eglGetProcAddress("glProvokingVertexANGLE");
    if (provoking != NULL) {
      GLint mode = 0;
      provoking(0x8E4D /* GL_FIRST_VERTEX_CONVENTION_ANGLE */);
      glGetIntegerv(0x8E4F /* GL_PROVOKING_VERTEX_ANGLE */, &mode);
      check("glProvokingVertexANGLE(GL_FIRST_VERTEX_CONVENTION_ANGLE) is read back",
            glGetError() == GL_NO_ERROR && mode == 0x8E4D);
      provoking(0x8E4E /* GL_LAST_VERTEX_CONVENTION_ANGLE */);
      glGetIntegerv(0x8E4F, &mode);
      check("glProvokingVertexANGLE(GL_LAST_VERTEX_CONVENTION_ANGLE) is read back",
            glGetError() == GL_NO_ERROR && mode == 0x8E4E);
    }
  }
  if (has_extension(extensions, "GL_EXT_disjoint_timer_query")) {
    typedef void (*GetInteger64)(GLenum, GLint64*);
    GetInteger64 get = (GetInteger64)eglGetProcAddress("glGetInteger64vEXT");
    if (get != NULL) {
      while (glGetError() != GL_NO_ERROR) {
      }
      GLint64 disjoint = -1;
      get(0x8FBB /* GL_GPU_DISJOINT_EXT */, &disjoint);
      check("glGetInteger64vEXT(GL_GPU_DISJOINT_EXT) answers 0 or 1",
            glGetError() == GL_NO_ERROR && (disjoint == 0 || disjoint == 1));
    }
  }

  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(display, context);
  eglDestroySurface(display, surface);
  return es;
}

int gesher_selftest(void) {
  printf("gl_extension_procs: entry points of the advertised GL extensions\n");
  int es = check_display(eglGetDisplay(EGL_DEFAULT_DISPLAY), "the default display");
  if (es == 2) {
    // No ES 3 on the default display (no GPU): the ES 3-only extensions on
    // ANGLE's uncapped WARP display, so they stay covered (test-only).
    check("an ES 3 context on ANGLE's uncapped WARP display",
          check_display(angle_warp_display(), "ANGLE's WARP display, uncapped (test-only)") == 3);
  }
  if (failures == 0) printf("gl_extension_procs: all passed\n");
  return failures;
}
