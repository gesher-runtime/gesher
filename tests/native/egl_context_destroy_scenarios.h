// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// eglDestroyContext on one thread while another thread draws with its own
// context of the same display (issue #485: Moovit's maps, MapLibre GL in
// several GLSurfaceViews, each with its own GL thread, on the one default
// display).
//
// ANGLE's Direct3D 11 back end keeps one state cache per display
// (StateManager11) for all contexts. Gesher builds ANGLE with
// angle_force_context_check_every_call (compat/angle/args.gn): a GL call
// marks all of its context's state dirty only when another context made the
// previous GL call. eglDestroyContext makes the doomed context current inside
// ANGLE, which empties the shared cache (the draw framebuffer, executable and
// vertex array), and switches back without recording a context switch. When
// the destroying thread has no context current, the context that made the
// previous GL call is not marked dirty again, and its next draw used the
// emptied cache: a NULL Framebuffer11 in StateManager11::updateState, "host
// fault reading 0x8" at libGLESv2.dll+0x1ebea2.
// compat/angle/patches/0001-forget-last-context-on-destroy.patch fixes it.
//
// The scenarios step two threads deterministically (one step at a time, the
// other thread waiting), so every run makes the same sequence of calls:
//
//   A: context A and a pbuffer current, draws with program 1
//   B: context X and a pbuffer current, draws, eglMakeCurrent(EGL_NO_CONTEXT)
//   A: glClear (A is marked dirty, the clear applies the draw framebuffer
//      and leaves the program, vertex array and blend state pending)
//   B: eglDestroySurface(X's pbuffer), eglDestroyContext(X)
//   A: [glUseProgram(program 2)], glDrawArrays, glReadPixels: every pixel
//      has the color of the program used
//
// Without the patch the last draw dereferences the emptied cache (a host
// access violation); the third scenario, where thread B has another context
// current while it destroys X, passed before as well (restoring B's context
// records a switch) and guards that path.
//
// The same source runs as a Windows program against ANGLE's Microsoft-ABI
// entry points (tests/native/egl_context_destroy_test.cc, track
// 40-native-libs) and as an NDK JNI library for x86_64 and arm64-v8a in
// dalvikvm, whose GL calls go through Gesher's EGL/GLES thunks and, for
// arm64, the translator (tests/native2/egl_context_destroy_jni.cpp, track
// 44-native2).
#pragma once

#if defined(_WIN32) && !defined(EGL_NO_PLATFORM_SPECIFIC_TYPES)
#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#endif
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#define EGL_CONTEXT_DESTROY_FUNCTIONS(X)                                                      \
  X(eglChooseConfig) X(eglCreatePbufferSurface) X(eglCreateContext) X(eglMakeCurrent)         \
  X(eglDestroySurface) X(eglDestroyContext) X(eglReleaseThread)                               \
  X(glCreateShader) X(glShaderSource) X(glCompileShader) X(glGetShaderiv) X(glCreateProgram)  \
  X(glAttachShader) X(glBindAttribLocation) X(glLinkProgram) X(glGetProgramiv)                \
  X(glUseProgram) X(glDeleteProgram) X(glDeleteShader) X(glGenBuffers) X(glBindBuffer)        \
  X(glBufferData) X(glDeleteBuffers) X(glVertexAttribPointer) X(glEnableVertexAttribArray)    \
  X(glViewport) X(glClearColor) X(glClear) X(glDrawArrays) X(glReadPixels) X(glFinish)        \
  X(glGetError)

namespace egl_context_destroy {

// The entry points, filled by the program: ANGLE's own (gesher_egl_proc) on
// Windows, the NDK's libEGL/libGLESv2 imports in an Android program.
struct Api {
#define EGL_CONTEXT_DESTROY_DECLARE(name) decltype(&::name) name = nullptr;
  EGL_CONTEXT_DESTROY_FUNCTIONS(EGL_CONTEXT_DESTROY_DECLARE)
#undef EGL_CONTEXT_DESTROY_DECLARE
};

constexpr int kSize = 8;

struct Color {
  unsigned char r, g, b;
};
constexpr Color kRed{255, 0, 0};
constexpr Color kGreen{0, 255, 0};
constexpr Color kBlue{0, 0, 255};
constexpr Color kYellow{255, 255, 0};

class Run {
 public:
  // Lines go to stdout, or, with a transcript (a JNI library, whose own
  // stdout an app does not show), into the transcript, with the progress
  // lines also on stderr so that a crash still shows where it happened.
  Run(const Api& api, EGLDisplay display, const char* tag, std::string* transcript = nullptr)
      : gl(api), display_(display), tag_(tag), transcript_(transcript) {}

  void Emit(bool progress, const char* format, ...) {
    char line[320];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (transcript_ == nullptr) {
      std::printf("%s\n", line);
      std::fflush(stdout);
      return;
    }
    *transcript_ += line;
    *transcript_ += '\n';
    if (progress) std::fprintf(stderr, "%s\n", line);
  }

  // Emits "<tag>: ok: ..." or "<tag>: FAILED: ..." and counts failures.
  bool Check(bool success, const char* format, ...) {
    char message[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    Emit(false, "%s: %s: %s", tag_, success ? "ok" : "FAILED", message);
    if (!success) ++failures_;
    return success;
  }
  int failures() const { return failures_; }
  const char* tag() const { return tag_; }

  const Api& gl;

  bool ChooseConfig(EGLConfig* config) {
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                 EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                 EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLint count = 0;
    return gl.eglChooseConfig(display_, attributes, config, 1, &count) == EGL_TRUE && count == 1;
  }

  // A pbuffer and an ES 2 context on it, made current on the calling thread.
  struct Context {
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
  };
  bool Create(EGLConfig config, Context* out) {
    const EGLint size[] = {EGL_WIDTH, kSize, EGL_HEIGHT, kSize, EGL_NONE};
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    out->surface = gl.eglCreatePbufferSurface(display_, config, size);
    out->context = gl.eglCreateContext(display_, config, EGL_NO_CONTEXT, version);
    return out->surface != EGL_NO_SURFACE && out->context != EGL_NO_CONTEXT && Current(*out);
  }
  bool Current(const Context& c) {
    if (gl.eglMakeCurrent(display_, c.surface, c.surface, c.context) != EGL_TRUE) return false;
    gl.glViewport(0, 0, kSize, kSize);
    return true;
  }
  bool Release() {
    return gl.eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE;
  }
  bool Destroy(Context* c) {
    bool surface = gl.eglDestroySurface(display_, c->surface) == EGL_TRUE;
    bool context = gl.eglDestroyContext(display_, c->context) == EGL_TRUE;
    c->surface = EGL_NO_SURFACE;
    c->context = EGL_NO_CONTEXT;
    return surface && context;
  }

  // A program that fills with one color, and a full-viewport quad (two
  // triangles) in a buffer at attribute 0, in the current context.
  GLuint Program(Color color) {
    static const char* const kVertex =
        "attribute vec2 position;\n"
        "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";
    char fragment[160];
    std::snprintf(fragment, sizeof(fragment),
                  "precision mediump float;\n"
                  "void main() { gl_FragColor = vec4(%d.0, %d.0, %d.0, 1.0); }\n",
                  color.r / 255, color.g / 255, color.b / 255);
    GLuint vs = Shader(GL_VERTEX_SHADER, kVertex);
    GLuint fs = Shader(GL_FRAGMENT_SHADER, fragment);
    GLuint program = gl.glCreateProgram();
    gl.glAttachShader(program, vs);
    gl.glAttachShader(program, fs);
    gl.glBindAttribLocation(program, 0, "position");
    gl.glLinkProgram(program);
    gl.glDeleteShader(vs);
    gl.glDeleteShader(fs);
    GLint linked = GL_FALSE;
    gl.glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
      gl.glDeleteProgram(program);
      return 0;
    }
    return program;
  }
  GLuint Quad() {
    static const GLfloat kQuad[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
    GLuint buffer = 0;
    gl.glGenBuffers(1, &buffer);
    gl.glBindBuffer(GL_ARRAY_BUFFER, buffer);
    gl.glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
    gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    gl.glEnableVertexAttribArray(0);
    return buffer;
  }
  void Draw() { gl.glDrawArrays(GL_TRIANGLES, 0, 6); }

  // Every pixel of the current draw surface has `color`.
  bool Pixels(Color color) {
    unsigned char pixels[kSize * kSize * 4] = {};
    gl.glReadPixels(0, 0, kSize, kSize, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (gl.glGetError() != GL_NO_ERROR) return false;
    for (int i = 0; i < kSize * kSize; ++i) {
      const unsigned char* p = pixels + 4 * i;
      if (p[0] != color.r || p[1] != color.g || p[2] != color.b) return false;
    }
    return true;
  }

 private:
  GLuint Shader(GLenum type, const char* source) {
    GLuint shader = gl.glCreateShader(type);
    gl.glShaderSource(shader, 1, &source, nullptr);
    gl.glCompileShader(shader);
    return shader;
  }

  EGLDisplay display_;
  const char* tag_;
  std::string* transcript_;
  int failures_ = 0;
};

// A second thread that runs one step at a time: Step() hands it a function
// and returns when it has run, so the two threads never run at once and the
// order of their calls is fixed.
class Stepper {
 public:
  Stepper() : thread_([this] { Loop(); }) {}
  ~Stepper() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
  }
  Stepper(const Stepper&) = delete;
  Stepper& operator=(const Stepper&) = delete;

  void Step(std::function<void()> step) {
    std::unique_lock<std::mutex> lock(mutex_);
    step_ = std::move(step);
    done_ = false;
    wake_.notify_all();
    wake_.wait(lock, [this] { return done_; });
  }

 private:
  void Loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      wake_.wait(lock, [this] { return quit_ || static_cast<bool>(step_); });
      if (!step_) return;
      std::function<void()> step = std::move(step_);
      step_ = nullptr;
      lock.unlock();
      step();
      lock.lock();
      done_ = true;
      wake_.notify_all();
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::function<void()> step_;
  bool done_ = true;
  bool quit_ = false;
  std::thread thread_;  // last: it starts with the members above initialized
};

enum class Variant {
  kUseProgram,         // A switches to program 2 before the draw
  kSameProgram,        // A draws again with program 1
  kOtherContextOnB,    // B has context C current while it destroys X
};

inline const char* Name(Variant variant) {
  switch (variant) {
    case Variant::kUseProgram: return "glUseProgram then draw";
    case Variant::kSameProgram: return "draw with the same program";
    case Variant::kOtherContextOnB: return "destroying thread has another context current";
  }
  return "?";
}

inline void Scenario(Run& run, EGLConfig config, Variant variant) {
  const Api& gl = run.gl;
  const char* name = Name(variant);
  run.Emit(true, "-- scenario: %s", name);
  Stepper b;

  // A: its context and two programs, a first draw.
  Run::Context a;
  if (!run.Check(run.Create(config, &a), "%s: thread A's context is current", name)) return;
  GLuint red = run.Program(kRed);
  GLuint green = run.Program(kGreen);
  GLuint quad = run.Quad();
  run.Check(red != 0 && green != 0, "%s: thread A links two programs", name);
  gl.glUseProgram(red);
  run.Draw();
  run.Check(run.Pixels(kRed), "%s: thread A draws red", name);

  // B: context X draws, then is released (as GLSurfaceView's GL thread
  // does before it destroys its surface and context).
  Run::Context x, c;
  b.Step([&] {
    bool ok = run.Create(config, &x);
    GLuint blue = ok ? run.Program(kBlue) : 0;
    GLuint buffer = ok ? run.Quad() : 0;
    if (blue != 0) {
      gl.glUseProgram(blue);
      run.Draw();
    }
    run.Check(ok && blue != 0 && run.Pixels(kBlue), "%s: thread B's context X draws blue", name);
    gl.glDeleteBuffers(1, &buffer);
    gl.glDeleteProgram(blue);
    if (variant == Variant::kOtherContextOnB) {
      run.Check(run.Create(config, &c), "%s: thread B makes context C current", name);
    } else {
      run.Check(run.Release(), "%s: thread B releases X (eglMakeCurrent EGL_NO_CONTEXT)", name);
    }
  });

  // A: a GL call after B's, so A is the context of the last GL call.
  gl.glClearColor(0, 0, 0, 1);
  gl.glClear(GL_COLOR_BUFFER_BIT);
  run.Check(run.Pixels(Color{0, 0, 0}), "%s: thread A clears", name);

  // B: destroys X's surface and X, which is current nowhere.
  b.Step([&] { run.Check(run.Destroy(&x), "%s: thread B destroys X's pbuffer and X", name); });

  // A: the draw that used ANGLE's emptied state cache.
  run.Emit(true, "%s: thread A draws after X was destroyed", name);
  Color expected = kRed;
  if (variant == Variant::kUseProgram) {
    gl.glUseProgram(green);
    expected = kGreen;
  }
  run.Draw();
  run.Check(run.Pixels(expected) && gl.glGetError() == GL_NO_ERROR,
            "%s: thread A's draw after X's destruction has its own program's color", name);

  if (variant == Variant::kOtherContextOnB) {
    b.Step([&] {
      GLuint yellow = run.Program(kYellow);
      GLuint buffer = run.Quad();
      gl.glUseProgram(yellow);
      run.Draw();
      run.Check(yellow != 0 && run.Pixels(kYellow), "%s: thread B's context C still draws", name);
      gl.glDeleteBuffers(1, &buffer);
      gl.glDeleteProgram(yellow);
      run.Check(run.Release() && run.Destroy(&c), "%s: thread B destroys C", name);
    });
  }
  b.Step([&] { gl.eglReleaseThread(); });

  // A: draws once more, then cleans up.
  gl.glUseProgram(red);
  run.Draw();
  run.Check(run.Pixels(kRed), "%s: thread A draws red again", name);
  gl.glDeleteBuffers(1, &quad);
  gl.glDeleteProgram(red);
  gl.glDeleteProgram(green);
  run.Check(run.Release() && run.Destroy(&a), "%s: thread A releases and destroys its context", name);
}

// Runs every scenario on `display`; emits "<tag> all passed" or
// "<tag> FAILED" and returns the process exit code.
inline int RunAll(const Api& api, EGLDisplay display, const char* tag,
                  std::string* transcript = nullptr) {
  Run run(api, display, tag, transcript);
  EGLConfig config = nullptr;
  if (run.Check(display != EGL_NO_DISPLAY && run.ChooseConfig(&config),
                "an RGBA8 pbuffer config for ES 2 on the default display")) {
    for (Variant variant : {Variant::kUseProgram, Variant::kSameProgram, Variant::kOtherContextOnB}) {
      Scenario(run, config, variant);
    }
  }
  run.Emit(true, "%s %s", tag, run.failures() ? "FAILED" : "all passed");
  return run.failures() ? 1 : 0;
}

}  // namespace egl_context_destroy
