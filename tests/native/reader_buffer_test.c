// gesher: AHardwareBuffer_fromHardwareBuffer on the HardwareBuffer of an
// ImageReader image, as Flutter's raster thread imports a platform view's
// frame (tests/dex-graphics/gesher/ImageReaderPrivateTest.java loads this
// library and passes it Image.getHardwareBuffer()).
//
//   nativeInspect(hardwareBuffer, probes, out): imports the buffer, describes
//     it, reads the pixels at probes (x, y pairs) through a CPU lock, then
//     makes an EGLImage of it when an EGL context is current, binds it to a
//     texture (glEGLImageTargetTexture2DOES) and reads the probes back through
//     a framebuffer. Fills out with {status, width, height, format, usage (low
//     bits), same buffer both times, EGLImage made (1), AHardwareBuffer_getId
//     (low, high), usage (high bits), GL status (1 = read back), pixel per
//     probe..., GL pixel per probe...}; every value is checked in Java.
//
// Only JNI and the NDK's AHardwareBuffer/EGL entry points, no libc.
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

enum { kStatus, kWidth, kHeight, kFormat, kUsage, kSame, kImage, kIdLo, kIdHi, kUsageHi, kGl, kPixels };

JNIEXPORT jint JNICALL Java_gesher_ImageReaderPrivateTest_nativeInspect(
    JNIEnv* env, jclass clazz, jobject hardware_buffer, jintArray probes, jintArray out) {
  (void)clazz;
  jsize probe_count = (*env)->GetArrayLength(env, probes) / 2;
  jint p[64];
  jint o[kPixels + 64];
  if (probe_count > 32) return -1;
  (*env)->GetIntArrayRegion(env, probes, 0, probe_count * 2, p);
  for (int i = 0; i < kPixels + probe_count * 2; i++) o[i] = 0;
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
        o[kPixels + i] = (jint)((unsigned)px[0] | (unsigned)px[1] << 8 | (unsigned)px[2] << 16 |
                                (unsigned)px[3] << 24);
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
        // Sampled as a texture: what Flutter's raster thread does with it.
        GLuint texture = 0, framebuffer = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)image);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        if (glGetError() == GL_NO_ERROR &&
            glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
          o[kGl] = 1;
          for (int i = 0; i < probe_count; i++) {
            unsigned char px[4] = {0, 0, 0, 0};
            glReadPixels(p[i * 2], p[i * 2 + 1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            o[kPixels + probe_count + i] = (jint)((unsigned)px[0] | (unsigned)px[1] << 8 |
                                                  (unsigned)px[2] << 16 | (unsigned)px[3] << 24);
          }
          if (glGetError() != GL_NO_ERROR) o[kGl] = 0;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture);
        eglDestroyImageKHR(display, image);
      }
    }
    AHardwareBuffer_release(buffer);
  }
  (*env)->SetIntArrayRegion(env, out, 0, kPixels + probe_count * 2, o);
  return o[kStatus];
}
