// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#ifndef CODEC_INPUT_CPU_WINDOW
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#endif

#include <inttypes.h>
#include <stdio.h>

#ifndef CODEC_STANDALONE
#include <jni.h>
#endif

enum { kWidth = 128, kHeight = 96 };

static bool Produce(ANativeWindow* window) {
#ifdef CODEC_INPUT_CPU_WINDOW
  ANativeWindow_Buffer buffer = {0};
  if (ANativeWindow_lock(window, &buffer, NULL) != 0) return false;
  const bool drawable = buffer.width == kWidth && buffer.height == kHeight &&
      buffer.stride >= kWidth && buffer.bits != NULL && buffer.format == WINDOW_FORMAT_RGBA_8888;
  if (drawable) {
    uint32_t* pixels = buffer.bits;
    for (int y = 0; y < buffer.height; ++y)
      for (int x = 0; x < buffer.width; ++x)
        pixels[y * buffer.stride + x] = UINT32_C(0xff808080);
  }
  return ANativeWindow_unlockAndPost(window) == 0 && drawable;
#else
  const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) return false;
  const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                              EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                              EGL_RECORDABLE_ANDROID, 1, EGL_NONE};
  const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLConfig config = NULL;
  EGLint actual = 0;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
  bool success = false;
  if (eglChooseConfig(display, attributes, &config, 1, &actual) && actual == 1 &&
      eglBindAPI(EGL_OPENGL_ES_API)) {
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
    surface = eglCreateWindowSurface(display, config, window, NULL);
    if (context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE &&
        eglMakeCurrent(display, surface, surface, context)) {
      glViewport(0, 0, kWidth, kHeight);
      glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      success = glGetError() == GL_NO_ERROR && eglSwapBuffers(display, surface);
      eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
  }
  if (!success) printf("NDK codec input EGL error: 0x%x\n", eglGetError());
  if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
  if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
  eglTerminate(display);
  return success;
#endif
}

static bool HasSlice(const uint8_t* bytes, size_t length) {
  for (size_t i = 0; i + 3 < length; ++i) {
    if (bytes[i] != 0 || bytes[i + 1] != 0 || bytes[i + 2] != 1) continue;
    const int nal_type = bytes[i + 3] & 31;
    if (nal_type == 1 || nal_type == 5) return true;
  }
  return false;
}

static int RunInput(void) {
  AMediaCodec* codec = AMediaCodec_createEncoderByType("video/avc");
  AMediaFormat* format = AMediaFormat_new();
  ANativeWindow* first = NULL;
  ANativeWindow* replacement = NULL;
  bool passed = false, eos = false;
  int pictures = 0;
#define VERIFY(test) do { \
  if (!(test)) { puts("NDK codec input failure: " #test); goto cleanup; } \
} while (0)

  VERIFY(codec != NULL && format != NULL);
  VERIFY(AMediaCodec_createInputSurface(codec, &first) == AMEDIA_ERROR_UNKNOWN && first == NULL);
  VERIFY(AMediaCodec_signalEndOfInputStream(codec) == AMEDIA_ERROR_INVALID_OPERATION);
  AMediaFormat_setString(format, "mime", "video/avc");
  AMediaFormat_setInt32(format, "width", kWidth);
  AMediaFormat_setInt32(format, "height", kHeight);
  AMediaFormat_setInt32(format, "color-format", 0x7f000789);
  AMediaFormat_setInt32(format, "bitrate", 200000);
  AMediaFormat_setInt32(format, "frame-rate", 30);
  AMediaFormat_setInt32(format, "i-frame-interval", 1);
  VERIFY(AMediaCodec_configure(codec, format, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) == AMEDIA_OK);
  VERIFY(AMediaCodec_createInputSurface(codec, &first) == AMEDIA_OK && first != NULL);
  VERIFY(AMediaCodec_createInputSurface(codec, &replacement) == AMEDIA_OK &&
         replacement != NULL && replacement != first);
  VERIFY(ANativeWindow_getWidth(replacement) == kWidth && ANativeWindow_getHeight(replacement) == kHeight);
  ANativeWindow_release(first);
  first = NULL;
  VERIFY(AMediaCodec_signalEndOfInputStream(codec) == AMEDIA_ERROR_INVALID_OPERATION);
  VERIFY(AMediaCodec_start(codec) == AMEDIA_OK);
  VERIFY(AMediaCodec_dequeueInputBuffer(codec, 0) == AMEDIA_ERROR_UNKNOWN);
  VERIFY(Produce(replacement));
  ANativeWindow_release(replacement);
  replacement = NULL;
  VERIFY(AMediaCodec_signalEndOfInputStream(codec) == AMEDIA_OK);
  VERIFY(AMediaCodec_signalEndOfInputStream(codec) == AMEDIA_ERROR_INVALID_OPERATION);

  for (int attempt = 0; attempt < 24 && !eos; ++attempt) {
    AMediaCodecBufferInfo info = {0};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, 250000);
    if (index < 0) {
      VERIFY(index >= AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED);
      continue;
    }
    if (info.size > 0 && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0) {
      const uint8_t* bytes = AMediaCodec_getOutputBuffer(codec, (size_t)index, NULL);
      VERIFY(bytes != NULL && info.offset >= 0 && HasSlice(bytes + info.offset, (size_t)info.size));
      printf("NDK codec input encoded AVC picture: bytes=%d pts=%" PRId64 "\n",
             info.size, info.presentationTimeUs);
      ++pictures;
    }
    eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
    VERIFY(AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false) == AMEDIA_OK);
  }
  VERIFY(eos && pictures == 1);
  VERIFY(AMediaCodec_stop(codec) == AMEDIA_OK);
  VERIFY(AMediaCodec_signalEndOfInputStream(codec) == AMEDIA_ERROR_INVALID_OPERATION);
  passed = true;

cleanup:
  if (first != NULL) ANativeWindow_release(first);
  if (replacement != NULL) ANativeWindow_release(replacement);
  if (codec != NULL) AMediaCodec_delete(codec);
  if (format != NULL) AMediaFormat_delete(format);
  if (passed) puts("NDK codec input Surface: all passed");
  return passed ? 0 : 1;
#undef VERIFY
}

#ifdef CODEC_STANDALONE
int main(void) { return RunInput(); }
#else
JNIEXPORT jint JNICALL Java_gesher_NdkCodecTest_run(JNIEnv* env, jclass type) {
  (void)env;
  (void)type;
  return RunInput();
}
#endif
