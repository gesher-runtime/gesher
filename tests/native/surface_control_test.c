// gesher: AHardwareBuffer, ASurfaceControl and the EGL native buffer and
// fence extensions from an NDK library (ADR 0089), driven by
// surface_control_host_test.exe (tests/native/surface_control_host_test.cc),
// which gives it a Gesher window and reads the composed frame back.
//
//   sc_test_run(window, out, n): the AHardwareBuffer, EGL and SurfaceControl
//     checks; leaves a composed frame in the window (see kExpected in the host);
//   sc_test_finish(out, n): takes the layers off the window (an empty frame)
//     and releases everything.
// Each returns the number of failed checks; every check prints a line.
//
// Built with the NDK for API 31 (setCrop/setPosition/setOnCommit,
// AHardwareBuffer_getId); setBufferWithRelease (API 36) is looked up with dlsym.
#include <android/data_space.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <android/surface_control.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond, ...)                                    \
  do {                                                      \
    if (cond) {                                             \
      printf("surface_control: ok: " __VA_ARGS__);          \
    } else {                                                \
      printf("surface_control: FAIL: " __VA_ARGS__);        \
      g_failures++;                                         \
    }                                                       \
    printf("\n");                                           \
    fflush(stdout);                                         \
  } while (0)

static const uint64_t kCpu = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
static const uint64_t kGpu = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;

#define RGBA(r, g, b, a) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | (uint32_t)(a) << 24)
#define RED RGBA(255, 0, 0, 255)
#define GREEN RGBA(0, 255, 0, 255)
#define BLUE RGBA(0, 0, 255, 255)
#define MAGENTA RGBA(255, 0, 255, 255)
#define WHITE RGBA(255, 255, 255, 255)
#define YELLOW RGBA(255, 255, 0, 255)

static AHardwareBuffer* Allocate(uint32_t w, uint32_t h, uint64_t usage) {
  AHardwareBuffer_Desc d = {w, h, 1, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, usage, 0, 0, 0};
  AHardwareBuffer* b = NULL;
  int rc = AHardwareBuffer_allocate(&d, &b);
  if (rc != 0) {
    printf("surface_control: FAIL: allocate %ux%u usage 0x%llx: %d\n", w, h, (unsigned long long)usage, rc);
    g_failures++;
    return NULL;
  }
  return b;
}

// Fills a buffer through a CPU lock: color(x, y) per pixel.
static int Fill(AHardwareBuffer* b, uint32_t (*color)(uint32_t x, uint32_t y, uint32_t w, uint32_t h)) {
  AHardwareBuffer_Desc d;
  AHardwareBuffer_describe(b, &d);
  void* p = NULL;
  int rc = AHardwareBuffer_lock(b, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &p);
  if (rc != 0) return rc;
  for (uint32_t y = 0; y < d.height; ++y) {
    uint32_t* row = (uint32_t*)p + (size_t)y * d.stride;
    for (uint32_t x = 0; x < d.width; ++x) row[x] = color(x, y, d.width, d.height);
  }
  int32_t fence = 7;
  rc = AHardwareBuffer_unlock(b, &fence);
  if (rc == 0 && fence != -1) return -1000;
  return rc;
}

// How many pixels differ from color(x, y) (read through a CPU lock with `fence`).
static long Mismatches(AHardwareBuffer* b, int fence,
                       uint32_t (*color)(uint32_t x, uint32_t y, uint32_t w, uint32_t h)) {
  AHardwareBuffer_Desc d;
  AHardwareBuffer_describe(b, &d);
  void* p = NULL;
  if (AHardwareBuffer_lock(b, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, fence, NULL, &p) != 0) return -1;
  long wrong = 0;
  for (uint32_t y = 0; y < d.height; ++y) {
    const uint32_t* row = (const uint32_t*)p + (size_t)y * d.stride;
    for (uint32_t x = 0; x < d.width; ++x) wrong += row[x] != color(x, y, d.width, d.height);
  }
  AHardwareBuffer_unlock(b, NULL);
  return wrong;
}

static uint32_t Pattern1(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  (void)w; (void)h;
  return RGBA(x * 4, y * 8, (x ^ y) & 0xff, 255);
}
static uint32_t Pattern2(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  (void)w; (void)h;
  return RGBA(255 - x, 128, y, 200);
}
// What the GPU draws in GpuDraw: blue with a red rectangle (8..24, 4..12).
static uint32_t GpuPattern(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  (void)w; (void)h;
  return x >= 8 && x < 24 && y >= 4 && y < 12 ? RED : BLUE;
}
// The overlay's source: quadrants red, green / magenta, white.
static uint32_t Quadrants(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  if (y < h / 2) return x < w / 2 ? RED : GREEN;
  return x < w / 2 ? MAGENTA : WHITE;
}
static uint32_t Yellow(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  (void)x; (void)y; (void)w; (void)h;
  return YELLOW;
}

static int HasExtension(const char* list, const char* name) {
  size_t n = strlen(name);
  for (const char* p = list; p != NULL && (p = strstr(p, name)) != NULL; p += n) {
    if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return 1;
  }
  return 0;
}

// --- AHardwareBuffer -----------------------------------------------------------------------------

static void TestBuffers(void) {
  AHardwareBuffer_Desc d = {64, 32, 1, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, kCpu, 0, 0, 0};
  CHECK(AHardwareBuffer_isSupported(&d) == 1, "isSupported: RGBA8 64x32 CPU read/write");
  AHardwareBuffer_Desc bad = d;
  bad.usage |= AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT;
  CHECK(AHardwareBuffer_isSupported(&bad) == 0, "isSupported: protected content with CPU usage is not");
  bad = d;
  bad.format = AHARDWAREBUFFER_FORMAT_BLOB;
  CHECK(AHardwareBuffer_isSupported(&bad) == 0, "isSupported: a BLOB of height 32 is not");
  bad = d;
  bad.layers = 2;
  CHECK(AHardwareBuffer_isSupported(&bad) == 0, "isSupported: two layers are not (not served)");
  bad = d;
  bad.format = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
  CHECK(AHardwareBuffer_isSupported(&bad) == 0, "isSupported: YCbCr 4:2:0 is not (not served)");
  bad = d;
  bad.usage |= AHARDWAREBUFFER_USAGE_VIDEO_ENCODE;
  CHECK(AHardwareBuffer_isSupported(&bad) == 0, "isSupported: video encoder usage is not (not served)");
  AHardwareBuffer* none = NULL;
  CHECK(AHardwareBuffer_allocate(&bad, &none) != 0 && none == NULL, "allocate fails what isSupported refuses");

  AHardwareBuffer* a = Allocate(64, 32, kCpu);
  AHardwareBuffer* b = Allocate(64, 32, kCpu);
  if (a == NULL || b == NULL) return;
  AHardwareBuffer_Desc got;
  AHardwareBuffer_describe(a, &got);
  CHECK(got.width == 64 && got.height == 32 && got.layers == 1 && got.format == 1 && got.usage == kCpu &&
            got.stride >= 64 && got.rfu0 == 0 && got.rfu1 == 0,
        "describe: %ux%u layers %u format %u usage 0x%llx stride %u", got.width, got.height, got.layers,
        got.format, (unsigned long long)got.usage, got.stride);
  uint64_t ida = 0, idb = 0;
  CHECK(AHardwareBuffer_getId(a, &ida) == 0 && AHardwareBuffer_getId(b, &idb) == 0 && ida != 0 && ida != idb &&
            (ida >> 32) == (uint64_t)getpid(),
        "getId: 0x%llx and 0x%llx (pid << 32 | count)", (unsigned long long)ida, (unsigned long long)idb);
  CHECK(Fill(a, Pattern1) == 0, "lock for writing, fill, unlock (fence -1: done)");
  CHECK(Mismatches(a, -1, Pattern1) == 0, "lock for reading sees what was written");
  void* p = NULL;
  int32_t bpp = 0, bps = 0;
  CHECK(AHardwareBuffer_lockAndGetInfo(a, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &p, &bpp, &bps) == 0 &&
            p != NULL && bpp == 4 && bps == (int32_t)got.stride * 4,
        "lockAndGetInfo: %d bytes per pixel, %d per row", bpp, bps);
  AHardwareBuffer_unlock(a, NULL);
  AHardwareBuffer_Planes planes;
  memset(&planes, 0, sizeof planes);
  CHECK(AHardwareBuffer_lockPlanes(a, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &planes) == 0 &&
            planes.planeCount == 1 && planes.planes[0].data == p && planes.planes[0].pixelStride == 4 &&
            planes.planes[0].rowStride == got.stride * 4,
        "lockPlanes: %u plane(s), pixel stride %u, row stride %u", planes.planeCount,
        planes.planes[0].pixelStride, planes.planes[0].rowStride);
  AHardwareBuffer_unlock(a, NULL);
  CHECK(AHardwareBuffer_lock(a, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, -1, NULL, &p) == -EINVAL,
        "lock with a GPU usage flag: -EINVAL");
  CHECK(AHardwareBuffer_unlock(a, NULL) != 0, "unlock of a buffer that is not locked fails");
  AHardwareBuffer* write_only = Allocate(8, 8, AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY);
  if (write_only != NULL) {
    CHECK(AHardwareBuffer_lock(write_only, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &p) == -EINVAL,
          "lock for reading a buffer made without CPU read usage: -EINVAL");
    AHardwareBuffer_release(write_only);
  }

  // Sent over an AF_UNIX socket: the same memory and id on the other end.
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
    CHECK(0, "socketpair: errno %d", errno);
  } else {
    int rc = AHardwareBuffer_sendHandleToUnixSocket(a, sv[0]);
    if (getenv("GESHER_BINDER") != NULL) {
      AHardwareBuffer* r = NULL;
      int rr = rc == 0 ? AHardwareBuffer_recvHandleFromUnixSocket(sv[1], &r) : -1;
      CHECK(rc == 0 && rr == 0 && r != NULL, "send/recvHandleFromUnixSocket: %d, %d", rc, rr);
      if (r != NULL) {
        AHardwareBuffer_Desc rd;
        AHardwareBuffer_describe(r, &rd);
        uint64_t idr = 0;
        AHardwareBuffer_getId(r, &idr);
        CHECK(rd.width == 64 && rd.height == 32 && rd.format == 1 && rd.usage == kCpu && rd.stride == got.stride &&
                  idr == ida,
              "the received buffer: %ux%u format %u, the same id", rd.width, rd.height, rd.format);
        CHECK(Mismatches(r, -1, Pattern1) == 0, "the received buffer has the sender's contents");
        CHECK(Fill(r, Pattern2) == 0 && Mismatches(a, -1, Pattern2) == 0,
              "what the receiver writes, the sender reads (shared memory)");
        AHardwareBuffer_release(r);
      }
    } else {
      // Without a descriptor broker, SCM_RIGHTS is refused (ADR 0088).
      CHECK(rc < 0, "sendHandleToUnixSocket without a descriptor broker fails: %d", rc);
    }
    close(sv[0]);
    close(sv[1]);
  }
  AHardwareBuffer_release(a);
  AHardwareBuffer_release(b);
}

// --- EGL -----------------------------------------------------------------------------------------

static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLContext g_context = EGL_NO_CONTEXT;
static EGLSurface g_surface = EGL_NO_SURFACE;
static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC p_get_client_buffer;
static PFNEGLCREATEIMAGEKHRPROC p_create_image;
static PFNEGLDESTROYIMAGEKHRPROC p_destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_image_target;
static PFNEGLCREATESYNCKHRPROC p_create_sync;
static PFNEGLDESTROYSYNCKHRPROC p_destroy_sync;
static PFNEGLDUPNATIVEFENCEFDANDROIDPROC p_dup_fence;
static PFNEGLWAITSYNCKHRPROC p_wait_sync;
static PFNEGLCLIENTWAITSYNCKHRPROC p_client_wait;
static PFNEGLGETSYNCATTRIBKHRPROC p_sync_attrib;

static int SetUpEgl(void) {
  g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (g_display == EGL_NO_DISPLAY || !eglInitialize(g_display, NULL, NULL)) return 0;
  const EGLint config_attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                   EGL_NONE};
  EGLConfig config;
  EGLint n = 0;
  if (!eglChooseConfig(g_display, config_attribs, &config, 1, &n) || n < 1) return 0;
  const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, context_attribs);
  const EGLint surface_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  g_surface = eglCreatePbufferSurface(g_display, config, surface_attribs);
  if (g_context == EGL_NO_CONTEXT || g_surface == EGL_NO_SURFACE) return 0;
  if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) return 0;
  p_get_client_buffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
  p_create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  p_destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
  p_image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  p_create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
  p_destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
  p_dup_fence = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
  p_wait_sync = (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
  p_client_wait = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
  p_sync_attrib = (PFNEGLGETSYNCATTRIBKHRPROC)eglGetProcAddress("eglGetSyncAttribKHR");
  return p_get_client_buffer && p_create_image && p_destroy_image && p_image_target && p_create_sync &&
         p_destroy_sync && p_dup_fence && p_wait_sync && p_client_wait && p_sync_attrib;
}

// A native fence after the commands so far, as its fd (-1 on failure).
static int NativeFence(void) {
  const EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE};
  EGLSyncKHR sync = p_create_sync(g_display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
  if (sync == EGL_NO_SYNC_KHR) return -1;
  glFlush();
  int fd = p_dup_fence(g_display, sync);
  p_destroy_sync(g_display, sync);
  return fd;
}

// The buffer's image as a texture attached to a framebuffer (bound).
typedef struct {
  EGLImageKHR image;
  GLuint texture, framebuffer;
} Target;

static int Attach(AHardwareBuffer* b, Target* t) {
  memset(t, 0, sizeof *t);
  EGLClientBuffer cb = p_get_client_buffer(b);
  const EGLint attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
  t->image = p_create_image(g_display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attribs);
  if (t->image == EGL_NO_IMAGE_KHR) return 0;
  glGenTextures(1, &t->texture);
  glBindTexture(GL_TEXTURE_2D, t->texture);
  p_image_target(GL_TEXTURE_2D, (GLeglImageOES)t->image);
  glGenFramebuffers(1, &t->framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, t->framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->texture, 0);
  return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR;
}

static void Detach(Target* t) {
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &t->framebuffer);
  glDeleteTextures(1, &t->texture);
  if (t->image != EGL_NO_IMAGE_KHR) p_destroy_image(g_display, t->image);
  t->image = EGL_NO_IMAGE_KHR;
}

// glReadPixels of the bound framebuffer against color(x, y).
static long GlMismatches(uint32_t w, uint32_t h, uint32_t (*color)(uint32_t, uint32_t, uint32_t, uint32_t)) {
  uint32_t* px = malloc((size_t)w * h * 4);
  glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  long wrong = 0;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) wrong += px[(size_t)y * w + x] != color(x, y, w, h);
  }
  free(px);
  return wrong;
}

// Blue with a red rectangle (8..24, 4..12), drawn into the bound framebuffer.
static void GpuDraw(uint32_t w, uint32_t h) {
  glViewport(0, 0, (GLsizei)w, (GLsizei)h);
  glClearColor(0, 0, 1, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);
  glScissor(8, 4, 16, 8);
  glClearColor(1, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
}

static void TestEgl(void) {
  if (!SetUpEgl()) {
    CHECK(0, "EGL: display, ES 3 context and the extension entry points");
    return;
  }
  const char* ext = eglQueryString(g_display, EGL_EXTENSIONS);
  CHECK(HasExtension(ext, "EGL_ANDROID_get_native_client_buffer") &&
            HasExtension(ext, "EGL_ANDROID_image_native_buffer") &&
            HasExtension(ext, "EGL_ANDROID_native_fence_sync"),
        "EGL_EXTENSIONS has EGL_ANDROID_get_native_client_buffer, image_native_buffer, native_fence_sync (%s)",
        ext != NULL ? ext : "null");
  void* egl = dlopen("libEGL.so", RTLD_NOW);
  CHECK(egl != NULL && dlsym(egl, "eglGetNativeClientBufferANDROID") == (void*)p_get_client_buffer &&
            dlsym(egl, "eglDupNativeFenceFDANDROID") == (void*)p_dup_fence,
        "dlsym(libEGL.so) and eglGetProcAddress give the same entry points");

  AHardwareBuffer* b = Allocate(64, 32, kCpu | kGpu);
  if (b == NULL) return;
  CHECK(Fill(b, Pattern1) == 0, "GPU buffer: CPU contents before the import");
  Target t;
  CHECK(Attach(b, &t), "eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID), glEGLImageTargetTexture2DOES, a complete "
                       "framebuffer");
  if (t.image == EGL_NO_IMAGE_KHR) return;
  long wrong = GlMismatches(64, 32, Pattern1);
  CHECK(wrong == 0, "the GPU reads what the CPU wrote before the import (%ld wrong)", wrong);
  CHECK(Fill(b, Pattern2) == 0, "CPU writes after the import");
  wrong = GlMismatches(64, 32, Pattern2);
  CHECK(wrong == 0, "the GPU reads what the CPU wrote after the import (%ld wrong)", wrong);

  GpuDraw(64, 32);
  int fence = NativeFence();
  struct pollfd pfd = {fence, POLLIN, 0};
  CHECK(fence >= 0 && poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN) != 0,
        "eglDupNativeFenceFDANDROID: fd %d, signaled when given", fence);
  // A fence made from that fd: waited for by the server and the client.
  int copy = fence >= 0 ? dup(fence) : -1;
  const EGLint from_fd[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, copy, EGL_NONE};
  EGLSyncKHR imported = p_create_sync(g_display, EGL_SYNC_NATIVE_FENCE_ANDROID, from_fd);
  EGLint type = 0, condition = 0, status = 0;
  CHECK(imported != EGL_NO_SYNC_KHR && p_wait_sync(g_display, imported, 0) == EGL_TRUE &&
            p_client_wait(g_display, imported, 0, 0) == EGL_CONDITION_SATISFIED_KHR &&
            p_sync_attrib(g_display, imported, EGL_SYNC_TYPE_KHR, &type) &&
            p_sync_attrib(g_display, imported, EGL_SYNC_CONDITION_KHR, &condition) &&
            p_sync_attrib(g_display, imported, EGL_SYNC_STATUS_KHR, &status) &&
            type == EGL_SYNC_NATIVE_FENCE_ANDROID && condition == EGL_SYNC_NATIVE_FENCE_SIGNALED_ANDROID &&
            status == EGL_SIGNALED_KHR,
        "a native fence from an fd: eglWaitSyncKHR, eglClientWaitSyncKHR, type/condition/status");
  if (imported != EGL_NO_SYNC_KHR) p_destroy_sync(g_display, imported);
  // The CPU lock waits for the fence (and owns it): it sees what the GPU drew.
  wrong = Mismatches(b, fence, GpuPattern);
  CHECK(wrong == 0, "a CPU lock after the fence reads what the GPU drew (%ld wrong)", wrong);

  // Errors, as Android's EGL reports them.
  EGLClientBuffer cb = p_get_client_buffer(b);
  CHECK(p_create_image(g_display, g_context, EGL_NATIVE_BUFFER_ANDROID, cb, NULL) == EGL_NO_IMAGE_KHR &&
            eglGetError() == EGL_BAD_CONTEXT,
        "eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) with a context: EGL_BAD_CONTEXT");
  int not_a_buffer = 0;
  CHECK(p_create_image(g_display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer)&not_a_buffer, NULL) ==
                EGL_NO_IMAGE_KHR &&
            eglGetError() == EGL_BAD_PARAMETER,
        "eglCreateImageKHR of something that is not a buffer: EGL_BAD_PARAMETER");
  CHECK(p_get_client_buffer((const struct AHardwareBuffer*)&not_a_buffer) == NULL && eglGetError() == EGL_BAD_PARAMETER,
        "eglGetNativeClientBufferANDROID of something that is not a buffer: EGL_BAD_PARAMETER");
  AHardwareBuffer* cpu_only = Allocate(8, 8, kCpu);
  if (cpu_only != NULL) {
    CHECK(p_create_image(g_display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, p_get_client_buffer(cpu_only),
                         NULL) == EGL_NO_IMAGE_KHR &&
              eglGetError() == EGL_BAD_PARAMETER,
          "eglCreateImageKHR of a buffer without GPU usage: EGL_BAD_PARAMETER");
    AHardwareBuffer_release(cpu_only);
  }
  EGLImageKHR image = t.image;
  Detach(&t);
  CHECK(p_destroy_image(g_display, image) == EGL_FALSE && eglGetError() == EGL_BAD_PARAMETER,
        "eglDestroyImageKHR of a destroyed image: EGL_BAD_PARAMETER");
  AHardwareBuffer_release(b);
}

// --- SurfaceControl ------------------------------------------------------------------------------

typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  int count;
  pid_t thread;
  int64_t latch;
  size_t surfaces;
  int present_fence;
  int release_fences_ok;  // every surface's previous release fence is -1
  int64_t acquire[8];     // by the order of `watched`
  int order;              // the global order in which it ran
} Callback;

static ASurfaceControl* g_watched[8];
static int g_watched_count;
static int g_sequence;
static pthread_mutex_t g_sequence_mutex = PTHREAD_MUTEX_INITIALIZER;

static void InitCallback(Callback* c) {
  memset(c, 0, sizeof *c);
  pthread_mutex_init(&c->mutex, NULL);
  pthread_cond_init(&c->cond, NULL);
}

static void Record(Callback* c, ASurfaceTransactionStats* stats, int complete) {
  pthread_mutex_lock(&c->mutex);
  c->thread = gettid();
  c->latch = ASurfaceTransactionStats_getLatchTime(stats);
  ASurfaceControl** controls = NULL;
  size_t n = 0;
  ASurfaceTransactionStats_getASurfaceControls(stats, &controls, &n);
  c->surfaces = n;
  c->release_fences_ok = 1;
  if (complete) {
    c->present_fence = ASurfaceTransactionStats_getPresentFenceFd(stats);
    for (size_t i = 0; i < n; ++i) {
      int fd = ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, controls[i]);
      if (fd != -1) {
        c->release_fences_ok = 0;
        close(fd);
      }
    }
    for (int i = 0; i < g_watched_count; ++i) {
      c->acquire[i] = -2;
      for (size_t j = 0; j < n; ++j) {
        if (controls[j] == g_watched[i]) c->acquire[i] = ASurfaceTransactionStats_getAcquireTime(stats, controls[j]);
      }
    }
  }
  ASurfaceTransactionStats_releaseASurfaceControls(controls);
  pthread_mutex_lock(&g_sequence_mutex);
  c->order = ++g_sequence;
  pthread_mutex_unlock(&g_sequence_mutex);
  c->count++;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mutex);
}

static void OnCommit(void* context, ASurfaceTransactionStats* stats) { Record(context, stats, 0); }
static void OnComplete(void* context, ASurfaceTransactionStats* stats) { Record(context, stats, 1); }

static int Wait(Callback* c, int count) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 10;
  pthread_mutex_lock(&c->mutex);
  while (c->count < count) {
    if (pthread_cond_timedwait(&c->cond, &c->mutex, &deadline) == ETIMEDOUT) break;
  }
  int ok = c->count >= count;
  pthread_mutex_unlock(&c->mutex);
  return ok;
}

typedef struct {
  pthread_mutex_t mutex;
  int calls;
  int fence;
  int order;
} Release;

static void OnRelease(void* context, int fence) {
  Release* r = context;
  pthread_mutex_lock(&r->mutex);
  r->calls++;
  r->fence = fence;
  pthread_mutex_lock(&g_sequence_mutex);
  r->order = ++g_sequence;
  pthread_mutex_unlock(&g_sequence_mutex);
  pthread_mutex_unlock(&r->mutex);
}

static ASurfaceControl* g_root;
static ASurfaceControl* g_layers[5];  // background, under, overlay, color, hidden
static AHardwareBuffer* g_buffers[4];  // background, under, overlay, hidden

static int64_t Now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void TestSurfaceControl(ANativeWindow* window) {
  const int32_t w = ANativeWindow_getWidth(window), h = ANativeWindow_getHeight(window);
  g_root = ASurfaceControl_createFromWindow(window, "sc_test root");
  CHECK(g_root != NULL, "ASurfaceControl_createFromWindow on a %dx%d window", w, h);
  if (g_root == NULL) return;
  ASurfaceControl* bg = g_layers[0] = ASurfaceControl_create(g_root, "background");
  ASurfaceControl* under = g_layers[1] = ASurfaceControl_create(bg, "under the background");
  ASurfaceControl* overlay = g_layers[2] = ASurfaceControl_create(g_root, "overlay");
  ASurfaceControl* color = g_layers[3] = ASurfaceControl_create(g_root, "color");
  ASurfaceControl* hidden = g_layers[4] = ASurfaceControl_create(g_root, "hidden");
  CHECK(bg && under && overlay && color && hidden, "ASurfaceControl_create: five layers");
  if (!(bg && under && overlay && color && hidden)) return;

  // The background: drawn by the GPU, given with its fence.
  AHardwareBuffer* bg_buffer = g_buffers[0] =
      Allocate((uint32_t)w, (uint32_t)h, kGpu | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY);
  AHardwareBuffer* under_buffer = g_buffers[1] = Allocate(16, 16, kCpu | kGpu | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY);
  AHardwareBuffer* overlay_buffer = g_buffers[2] = Allocate(32, 16, kCpu | kGpu | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY);
  AHardwareBuffer* hidden_buffer = g_buffers[3] = Allocate(16, 16, kCpu | kGpu | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY);
  if (!bg_buffer || !under_buffer || !overlay_buffer || !hidden_buffer) return;
  Target t;
  int fence = -1;
  if (Attach(bg_buffer, &t)) {
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    fence = NativeFence();
  }
  CHECK(fence >= 0, "the background drawn by the GPU, with a native fence");
  Detach(&t);
  CHECK(Fill(under_buffer, Yellow) == 0 && Fill(overlay_buffer, Quadrants) == 0 && Fill(hidden_buffer, Yellow) == 0,
        "the other buffers written by the CPU");

  g_watched[0] = bg;
  g_watched[1] = overlay;
  g_watched_count = 2;
  ASurfaceTransaction* tx = ASurfaceTransaction_create();
  const ARect full = {0, 0, w, h};
  ASurfaceTransaction_setBuffer(tx, bg, bg_buffer, fence);
  ASurfaceTransaction_setGeometry(tx, bg, &full, &full, 0);
  ASurfaceTransaction_setBufferTransparency(tx, bg, ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
  ASurfaceTransaction_setZOrder(tx, bg, 0);
  // Below its opaque parent's content (z < 0): never seen.
  ASurfaceTransaction_setBuffer(tx, under, under_buffer, -1);
  const ARect under_source = {0, 0, 16, 16};
  ASurfaceTransaction_setGeometry(tx, under, &under_source, &full, 0);
  ASurfaceTransaction_setZOrder(tx, under, -1);
  // 32x16 rotated 90 degrees into 32x64 at (100, 20): magenta, red / white, green.
  const ARect overlay_source = {0, 0, 32, 16};
  const ARect overlay_dest = {100, 20, 132, 84};
  ASurfaceTransaction_setBuffer(tx, overlay, overlay_buffer, -1);
  ASurfaceTransaction_setGeometry(tx, overlay, &overlay_source, &overlay_dest, ANATIVEWINDOW_TRANSFORM_ROTATE_90);
  ASurfaceTransaction_setZOrder(tx, overlay, 1);
  // A white color layer cropped to (10, 100)-(30, 110).
  ASurfaceTransaction_setColor(tx, color, 1, 1, 1, 1, ADATASPACE_SRGB);
  const ARect crop = {10, 100, 30, 110};
  ASurfaceTransaction_setCrop(tx, color, &crop);
  ASurfaceTransaction_setZOrder(tx, color, 2);
  ASurfaceTransaction_setBuffer(tx, hidden, hidden_buffer, -1);
  ASurfaceTransaction_setGeometry(tx, hidden, &under_source, &full, 0);
  ASurfaceTransaction_setVisibility(tx, hidden, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
  ASurfaceTransaction_setZOrder(tx, hidden, 3);
  Callback commit, complete;
  InitCallback(&commit);
  InitCallback(&complete);
  ASurfaceTransaction_setOnCommit(tx, &commit, OnCommit);
  ASurfaceTransaction_setOnComplete(tx, &complete, OnComplete);
  const int64_t before = Now();
  ASurfaceTransaction_apply(tx);
  ASurfaceTransaction_delete(tx);
  CHECK(Wait(&commit, 1) && Wait(&complete, 1), "OnCommit and OnComplete called");
  CHECK(commit.order < complete.order, "OnCommit before OnComplete");
  CHECK(complete.thread != gettid() && commit.thread != gettid(), "callbacks on another thread");
  CHECK(complete.latch >= before && complete.latch <= Now() && commit.latch >= before,
        "latch times between apply and the callback (CLOCK_MONOTONIC)");
  CHECK(complete.surfaces == 5 && commit.surfaces == 5, "the stats name the 5 surfaces the transaction changed (%zu)",
        complete.surfaces);
  CHECK(complete.present_fence == -1, "no present fence (-1)");
  CHECK(complete.release_fences_ok, "previous release fences -1 (nothing replaced yet)");
  CHECK(complete.acquire[0] >= before && complete.acquire[1] == -1,
        "acquire time: when the background's fence was seen signaled; -1 for the overlay (no fence)");

  // A second buffer with a release callback, then the first again: the
  // second is released (fence -1) before the third transaction completes.
  typedef void (*SetBufferWithRelease)(ASurfaceTransaction*, ASurfaceControl*, AHardwareBuffer*, int, void*,
                                       void (*)(void*, int));
  SetBufferWithRelease with_release =
      (SetBufferWithRelease)dlsym(dlopen("libandroid.so", RTLD_NOW), "ASurfaceTransaction_setBufferWithRelease");
  AHardwareBuffer* second = Allocate(32, 16, kCpu | kGpu | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY);
  CHECK(with_release != NULL && second != NULL && Fill(second, Yellow) == 0,
        "ASurfaceTransaction_setBufferWithRelease (API 36) is there");
  if (with_release != NULL && second != NULL) {
    Release release = {PTHREAD_MUTEX_INITIALIZER, 0, 7, 0};
    Callback c2, c3;
    InitCallback(&c2);
    InitCallback(&c3);
    ASurfaceTransaction* t2 = ASurfaceTransaction_create();
    with_release(t2, overlay, second, -1, &release, OnRelease);
    ASurfaceTransaction_setOnComplete(t2, &c2, OnComplete);
    ASurfaceTransaction* t3 = ASurfaceTransaction_create();
    ASurfaceTransaction_setBuffer(t3, overlay, overlay_buffer, -1);
    ASurfaceTransaction_setOnComplete(t3, &c3, OnComplete);
    ASurfaceTransaction_apply(t2);
    ASurfaceTransaction_apply(t3);
    ASurfaceTransaction_delete(t2);
    ASurfaceTransaction_delete(t3);
    CHECK(Wait(&c2, 1) && Wait(&c3, 1) && c2.order < c3.order, "transactions complete in the order applied");
    pthread_mutex_lock(&release.mutex);
    CHECK(release.calls == 1 && release.fence == -1 && release.order < c3.order,
          "the replaced buffer's release callback: once, fence -1, before OnComplete (%d call(s))", release.calls);
    pthread_mutex_unlock(&release.mutex);
    CHECK(c2.surfaces == 1 && c2.release_fences_ok && c3.surfaces == 1 && c3.release_fences_ok,
          "each stats names the overlay, previous release fence -1");
    AHardwareBuffer_release(second);
  }
}

// --- entry points --------------------------------------------------------------------------------

int sc_test_run(ANativeWindow* window, char* out, size_t n) {
  g_failures = 0;
  TestBuffers();
  TestEgl();
  TestSurfaceControl(window);
  snprintf(out, n, "%d failure(s)", g_failures);
  return g_failures;
}

int sc_test_finish(char* out, size_t n) {
  g_failures = 0;
  if (g_root != NULL) {
    // Off the window: the window's frame becomes empty (transparent).
    Callback done;
    InitCallback(&done);
    ASurfaceTransaction* tx = ASurfaceTransaction_create();
    ASurfaceTransaction_reparent(tx, g_root, NULL);
    ASurfaceTransaction_setOnComplete(tx, &done, OnComplete);
    ASurfaceTransaction_apply(tx);
    ASurfaceTransaction_delete(tx);
    CHECK(Wait(&done, 1), "reparent to null completes");
    for (int i = 0; i < 5; ++i) {
      if (g_layers[i] != NULL) ASurfaceControl_release(g_layers[i]);
    }
    ASurfaceControl_release(g_root);
    g_root = NULL;
  }
  for (int i = 0; i < 4; ++i) {
    if (g_buffers[i] != NULL) AHardwareBuffer_release(g_buffers[i]);
    g_buffers[i] = NULL;
  }
  if (g_display != EGL_NO_DISPLAY) {
    eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(g_display, g_surface);
    eglDestroyContext(g_display, g_context);
  }
  snprintf(out, n, "%d failure(s)", g_failures);
  return g_failures;
}
