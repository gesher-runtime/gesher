// gesher: AHardwareBuffer_fromHardwareBuffer on the HardwareBuffer of an
// ImageReader image, as Flutter's raster thread imports a platform view's
// frame (tests/dex-graphics/gesher/ImageReaderPrivateTest.java loads this
// library and passes it Image.getHardwareBuffer()).
//
//   nativeInspect(hardwareBuffer, probes, out): imports the buffer, describes
//     it, reads the pixels at probes (x, y pairs) through a CPU lock, then,
//     when an EGL context is current, makes an EGLImage of it and reads the
//     probes back twice from the GPU: through a GL_TEXTURE_2D bound with
//     glEGLImageTargetTexture2DOES (framebuffer readback) and through a
//     GL_TEXTURE_EXTERNAL_OES sampled by a samplerExternalOES shader (Flutter's
//     path) into a framebuffer. Fills out with {status, width, height, format,
//     usage (low bits), same buffer both times, EGLImage made (1),
//     AHardwareBuffer_getId (low, high), usage (high bits), 2D readback ok (1),
//     external sampling ok (1), CPU pixel per probe..., 2D pixel per probe...,
//     external pixel per probe...}; every value is checked in Java.
//   nativeFenceFd(): a native fence fd of the current EGL display (an
//     EGL_SYNC_NATIVE_FENCE_ANDROID after glFlush), or -1.
//
// Only JNI and the NDK's AHardwareBuffer/EGL/GLES entry points, no libc.
#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <jni.h>
#include <stddef.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

enum {
  kStatus, kWidth, kHeight, kFormat, kUsage, kSame, kImage, kIdLo, kIdHi, kUsageHi, kGl2d, kGlExt,
  kPixels
};

static unsigned pack(const unsigned char* px) {
  return (unsigned)px[0] | (unsigned)px[1] << 8 | (unsigned)px[2] << 16 | (unsigned)px[3] << 24;
}

static void read_probes(const jint* p, int count, jint* out) {
  for (int i = 0; i < count; i++) {
    unsigned char px[4] = {0, 0, 0, 0};
    glReadPixels(p[i * 2], p[i * 2 + 1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    out[i] = (jint)pack(px);
  }
}

// The image as a GL_TEXTURE_2D, read back through a framebuffer.
static int read_2d(EGLImageKHR image, const jint* p, int count, jint* out) {
  GLuint texture = 0, framebuffer = 0;
  int ok = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)image);
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  if (glGetError() == GL_NO_ERROR && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
    read_probes(p, count, out);
    ok = glGetError() == GL_NO_ERROR;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &framebuffer);
  glDeleteTextures(1, &texture);
  return ok;
}

static GLuint compile(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  GLint compiled = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (!compiled) {
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

// The image as a GL_TEXTURE_EXTERNAL_OES drawn by a samplerExternalOES shader
// over the whole (width x height) framebuffer texture, then read back.
static int read_external(EGLImageKHR image, int width, int height, const jint* p, int count,
                         jint* out) {
  static const char* kVertex =
      "attribute vec2 pos; attribute vec2 uv; varying vec2 v;"
      "void main() { gl_Position = vec4(pos, 0.0, 1.0); v = uv; }";
  static const char* kFragment =
      "#extension GL_OES_EGL_image_external : require\n"
      "precision mediump float; varying vec2 v; uniform samplerExternalOES s;"
      "void main() { gl_FragColor = texture2D(s, v); }";
  static const GLfloat kPos[] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
  static const GLfloat kUv[] = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f};
  int ok = 0;
  GLuint vs = compile(GL_VERTEX_SHADER, kVertex), fs = compile(GL_FRAGMENT_SHADER, kFragment);
  GLuint program = glCreateProgram(), external = 0, target = 0, framebuffer = 0;
  if (vs != 0 && fs != 0) {
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "pos");
    glBindAttribLocation(program, 1, "uv");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked) {
      glGenTextures(1, &external);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, external);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)image);
      glGenTextures(1, &target);
      glBindTexture(GL_TEXTURE_2D, target);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glGenFramebuffers(1, &framebuffer);
      glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
      if (glGetError() == GL_NO_ERROR &&
          glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glViewport(0, 0, width, height);
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "s"), 0);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kPos);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, kUv);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        read_probes(p, count, out);
        ok = glGetError() == GL_NO_ERROR;
      }
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
  }
  glUseProgram(0);
  if (framebuffer != 0) glDeleteFramebuffers(1, &framebuffer);
  if (target != 0) glDeleteTextures(1, &target);
  if (external != 0) glDeleteTextures(1, &external);
  glDeleteProgram(program);
  if (vs != 0) glDeleteShader(vs);
  if (fs != 0) glDeleteShader(fs);
  return ok;
}

JNIEXPORT jint JNICALL Java_gesher_ImageReaderPrivateTest_nativeInspect(
    JNIEnv* env, jclass clazz, jobject hardware_buffer, jintArray probes, jintArray out) {
  (void)clazz;
  jsize probe_count = (*env)->GetArrayLength(env, probes) / 2;
  jint p[64];
  jint o[kPixels + 96];
  if (probe_count > 32) return -1;
  (*env)->GetIntArrayRegion(env, probes, 0, probe_count * 2, p);
  for (int i = 0; i < kPixels + probe_count * 3; i++) o[i] = 0;
  o[kStatus] = -1;

  AHardwareBuffer* buffer = AHardwareBuffer_fromHardwareBuffer(env, hardware_buffer);
  if (buffer != NULL) {
    o[kStatus] = 0;
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(buffer, &desc);
    o[kWidth] = (jint)desc.width;
    o[kHeight] = (jint)desc.height;
    o[kFormat] = (jint)desc.format;
    o[kUsage] = (jint)(desc.usage & 0xffffffffu);
    o[kUsageHi] = (jint)(desc.usage >> 32);
    uint64_t id = 0;
    if (AHardwareBuffer_getId(buffer, &id) == 0) {
      o[kIdLo] = (jint)(id & 0xffffffffu);
      o[kIdHi] = (jint)(id >> 32);
    }
    o[kSame] = AHardwareBuffer_fromHardwareBuffer(env, hardware_buffer) == buffer;
    // NDK: fromHardwareBuffer takes no reference; an app that keeps the
    // buffer acquires its own.
    AHardwareBuffer_acquire(buffer);
    void* bits = NULL;
    int locked = AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &bits);
    if (locked == 0 && bits != NULL) {
      const unsigned char* base = (const unsigned char*)bits;
      for (int i = 0; i < probe_count; i++) {
        const unsigned char* px = base + ((long)p[i * 2 + 1] * (long)desc.stride + p[i * 2]) * 4;
        o[kPixels + i] = (jint)pack(px);
      }
      AHardwareBuffer_unlock(buffer, NULL);
    } else {
      o[kStatus] = locked != 0 ? locked : -2;
    }
    EGLDisplay display = eglGetCurrentDisplay();
    if (display != EGL_NO_DISPLAY) {
      EGLClientBuffer client = eglGetNativeClientBufferANDROID(buffer);
      const EGLint attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
      EGLImageKHR image =
          client != NULL ? eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                             client, attribs)
                         : EGL_NO_IMAGE_KHR;
      if (image != EGL_NO_IMAGE_KHR) {
        o[kImage] = 1;
        o[kGl2d] = read_2d(image, p, probe_count, o + kPixels + probe_count);
        o[kGlExt] = read_external(image, (int)desc.width, (int)desc.height, p, probe_count,
                                  o + kPixels + probe_count * 2);
        eglDestroyImageKHR(display, image);
      }
    }
    AHardwareBuffer_release(buffer);
  }
  (*env)->SetIntArrayRegion(env, out, 0, kPixels + probe_count * 3, o);
  return o[kStatus];
}

JNIEXPORT jint JNICALL Java_gesher_ImageReaderPrivateTest_nativeFenceFd(JNIEnv* env, jclass clazz) {
  (void)env;
  (void)clazz;
  EGLDisplay display = eglGetCurrentDisplay();
  if (display == EGL_NO_DISPLAY) return -1;
  const EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE};
  EGLSyncKHR sync = eglCreateSyncKHR(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
  if (sync == EGL_NO_SYNC_KHR) return -1;
  glFlush();
  jint fd = eglDupNativeFenceFDANDROID(display, sync);
  eglDestroySyncKHR(display, sync);
  return fd;
}
