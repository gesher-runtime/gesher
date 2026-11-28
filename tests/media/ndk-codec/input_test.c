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

static media_status_t Configure(AMediaCodec* codec, AMediaFormat* format) {
  AMediaFormat_setString(format, "mime", "video/avc");
  AMediaFormat_setInt32(format, "width", kWidth);
  AMediaFormat_setInt32(format, "height", kHeight);
  AMediaFormat_setInt32(format, "color-format", 0x7f000789);
  AMediaFormat_setInt32(format, "bitrate", 200000);
  AMediaFormat_setInt32(format, "frame-rate", 30);
  AMediaFormat_setInt32(format, "i-frame-interval", 1);
  return AMediaCodec_configure(codec, format, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
}

static bool ReadPictureAndEos(AMediaCodec* codec) {
  bool eos = false;
  int pictures = 0;
  for (int attempt = 0; attempt < 24 && !eos; ++attempt) {
    AMediaCodecBufferInfo info = {0};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, 250000);
    if (index < 0) {
      if (index < AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) return false;
      continue;
    }
    bool valid = true;
    if (info.size > 0 && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0) {
      const uint8_t* bytes = AMediaCodec_getOutputBuffer(codec, (size_t)index, NULL);
      valid = bytes != NULL && info.offset >= 0 && HasSlice(bytes + info.offset, (size_t)info.size);
      if (valid) {
        printf("NDK codec input encoded AVC picture: bytes=%d pts=%" PRId64 "\n",
               info.size, info.presentationTimeUs);
        ++pictures;
      }
    }
    eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
    if (AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false) != AMEDIA_OK || !valid)
      return false;
  }
  return eos && pictures == 1;
}

static int RunInput(void) {
  AMediaCodec* codec = AMediaCodec_createEncoderByType("video/avc");
  AMediaFormat* format = AMediaFormat_new();
  ANativeWindow* first = NULL;
  ANativeWindow* replacement = NULL;
  bool passed = false;
#define VERIFY(test) do { \
  if (!(test)) { puts("NDK codec input failure: " #test); goto cleanup; } \
} while (0)

  VERIFY(codec != NULL && format != NULL);
  VERIFY(AMediaCodec_createInputSurface(codec, &first) == AMEDIA_ERROR_UNKNOWN && first == NULL);
  VERIFY(AMediaCodec_signalEndOfInputStream(codec) == AMEDIA_ERROR_INVALID_OPERATION);
  VERIFY(Configure(codec, format) == AMEDIA_OK);
  VERIFY(AMediaCodec_createInputSurface(codec, &first) == AMEDIA_OK && first != NULL);
#ifdef CODEC_INPUT_CPU_WINDOW
  VERIFY(AMediaCodec_setInputSurface(codec, first) == AMEDIA_ERROR_INVALID_PARAMETER);
#endif
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

  VERIFY(ReadPictureAndEos(codec));
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
}

static int RunPersistent(void) {
  AMediaCodec* first = AMediaCodec_createEncoderByType("video/avc");
  AMediaCodec* second = AMediaCodec_createEncoderByType("video/avc");
  AMediaFormat* format = AMediaFormat_new();
  ANativeWindow* window = NULL;
  bool passed = false;

  VERIFY(first != NULL && second != NULL && format != NULL);
  VERIFY(AMediaCodec_createPersistentInputSurface(NULL) == AMEDIA_ERROR_INVALID_PARAMETER);
  VERIFY(AMediaCodec_createPersistentInputSurface(&window) == AMEDIA_OK && window != NULL);
  VERIFY(AMediaCodec_setInputSurface(NULL, window) == AMEDIA_ERROR_INVALID_PARAMETER);
  VERIFY(AMediaCodec_setInputSurface(first, NULL) == AMEDIA_ERROR_INVALID_PARAMETER);
  VERIFY(AMediaCodec_setInputSurface(first, window) == AMEDIA_ERROR_UNKNOWN);
  VERIFY(Configure(first, format) == AMEDIA_OK);
  VERIFY(Configure(second, format) == AMEDIA_OK);
  VERIFY(AMediaCodec_setInputSurface(first, window) == AMEDIA_OK);
#ifdef CODEC_INPUT_CPU_WINDOW
  // The Windows queue grants a single exclusive lease at attachment time.
  VERIFY(AMediaCodec_setInputSurface(first, window) == AMEDIA_OK);
#endif
  VERIFY(AMediaCodec_start(first) == AMEDIA_OK);
#ifdef CODEC_INPUT_CPU_WINDOW
  VERIFY(AMediaCodec_setInputSurface(second, window) == AMEDIA_ERROR_UNKNOWN);
#endif
  VERIFY(Produce(window));
  VERIFY(AMediaCodec_signalEndOfInputStream(first) == AMEDIA_OK);
  VERIFY(ReadPictureAndEos(first));
  VERIFY(AMediaCodec_stop(first) == AMEDIA_OK);
  VERIFY(AMediaCodec_delete(first) == AMEDIA_OK);
  first = NULL;

  VERIFY(AMediaCodec_setInputSurface(second, window) == AMEDIA_OK);
  VERIFY(AMediaCodec_start(second) == AMEDIA_OK);
  VERIFY(Produce(window));
  ANativeWindow_release(window);
  window = NULL;
  VERIFY(AMediaCodec_signalEndOfInputStream(second) == AMEDIA_OK);
  VERIFY(ReadPictureAndEos(second));
  VERIFY(AMediaCodec_stop(second) == AMEDIA_OK);
  passed = true;

cleanup:
  if (first != NULL) AMediaCodec_delete(first);
  if (second != NULL) AMediaCodec_delete(second);
  if (window != NULL) ANativeWindow_release(window);
  if (format != NULL) AMediaFormat_delete(format);
  if (passed) puts("NDK codec persistent input Surface: all passed");
  return passed ? 0 : 1;
}
#undef VERIFY

static int RunInputs(void) {
  return RunInput() != 0 || RunPersistent() != 0;
}

#ifdef CODEC_STANDALONE
int main(void) { return RunInputs(); }
#else
JNIEXPORT jint JNICALL Java_gesher_NdkCodecTest_run(JNIEnv* env, jclass type) {
  (void)env;
  (void)type;
  return RunInputs();
}
#endif
