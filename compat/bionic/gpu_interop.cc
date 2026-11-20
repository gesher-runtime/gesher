// gesher: the process's GPU thread (ADR 0089). See gpu_interop.h.
#include "gpu_interop.h"

#include <gesher/egl_angle.h>

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif

namespace gesher::bionic::gpu {

namespace {

void Log(const char* text) {
  std::fprintf(stderr, "gesher GPU thread: %s\n", text);
  std::fflush(stderr);
}

const char kVertexShader[] =
    "attribute vec2 a_position;\n"  // window pixels, origin top left
    "attribute vec2 a_texcoord;\n"
    "uniform vec2 u_viewport;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    // Row 0 of the target is the top row: read back first, as memory has it.
    "  gl_Position = vec4(a_position / u_viewport * 2.0 - 1.0, 0.0, 1.0);\n"
    "  v_texcoord = a_texcoord;\n"
    "}\n";

const char kFragmentShader[] =
    "precision mediump float;\n"
    "uniform sampler2D u_texture;\n"
    "uniform vec4 u_color;\n"   // premultiplied
    "uniform float u_alpha;\n"
    "uniform float u_mode;\n"   // 0: the texture, 1: u_color
    "uniform float u_opaque;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "  vec4 c = u_mode > 0.5 ? u_color : texture2D(u_texture, v_texcoord);\n"
    "  if (u_opaque > 0.5) c.a = 1.0;\n"
    "  gl_FragColor = c * u_alpha;\n"
    "}\n";

}  // namespace

const Gl* Functions() {
  static const Gl* functions = [] {
    auto* gl = new Gl();
    bool ok = true;
#define GESHER_GPU_RESOLVE(name)                                                     \
  gl->name = reinterpret_cast<decltype(&::name)>(gesher_egl_proc(#name));            \
  if (gl->name == nullptr) {                                                         \
    std::fprintf(stderr, "gesher GPU thread: ANGLE lacks %s\n", #name);              \
    ok = false;                                                                      \
  }
    GESHER_GPU_GL_FUNCTIONS(GESHER_GPU_RESOLVE)
    GESHER_GPU_EGL_FUNCTIONS(GESHER_GPU_RESOLVE)
#undef GESHER_GPU_RESOLVE
    if (!ok) {
      std::fflush(stderr);
      delete gl;
      return static_cast<Gl*>(nullptr);
    }
    return gl;
  }();
  return functions;
}

struct Thread::Queue {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::function<void()>> tasks;
  std::thread::id id;
  bool started = false, ok = false;
};

Thread* Thread::Get() {
  static Thread* thread = [] {
    auto* t = new Thread();
    if (!t->Start()) {
      // Kept allocated: a failed start may leave the thread's queue referenced.
      return static_cast<Thread*>(nullptr);
    }
    return t;
  }();
  return thread;
}

bool Thread::Start() {
  gl_ = Functions();
  if (gl_ == nullptr) {
    Log("ANGLE is not available; AHardwareBuffer GPU usage and SurfaceControl composition are not");
    return false;
  }
  queue_ = new Queue();
  std::thread([this] { Main(); }).detach();
  std::unique_lock<std::mutex> lock(queue_->mutex);
  queue_->cv.wait(lock, [this] { return queue_->started; });
  return queue_->ok;
}

bool Thread::InitOnThread() {
  const Gl& g = *gl_;
  display_ = static_cast<EGLDisplay>(gesher_egl_default_display());
  if (display_ == EGL_NO_DISPLAY) {
    Log("no EGL display (no Direct3D 11 device)");
    return false;
  }
  g.eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint config_attribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                   EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                   EGL_ALPHA_SIZE, 8, EGL_NONE};
  EGLint count = 0;
  if (g.eglChooseConfig(display_, config_attribs, &config_, 1, &count) != EGL_TRUE || count < 1) {
    Log("no RGBA8 ES 3 pbuffer config");
    return false;
  }
  const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  context_ = g.eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attribs);
  if (context_ == EGL_NO_CONTEXT) {
    std::fprintf(stderr, "gesher GPU thread: eglCreateContext failed: 0x%x\n", g.eglGetError());
    return false;
  }
  const EGLint surface_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  surface_ = g.eglCreatePbufferSurface(display_, config_, surface_attribs);
  if (surface_ == EGL_NO_SURFACE ||
      g.eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
    std::fprintf(stderr, "gesher GPU thread: no current context: 0x%x\n", g.eglGetError());
    return false;
  }
  g.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size_);

  auto compile = [&](GLenum type, const char* source) -> GLuint {
    GLuint shader = g.glCreateShader(type);
    g.glShaderSource(shader, 1, &source, nullptr);
    g.glCompileShader(shader);
    GLint ok = 0;
    g.glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[512] = {};
      g.glGetShaderInfoLog(shader, sizeof log, nullptr, log);
      std::fprintf(stderr, "gesher GPU thread: shader: %s\n", log);
      g.glDeleteShader(shader);
      return 0;
    }
    return shader;
  };
  GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
  GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
  if (vs == 0 || fs == 0) return false;
  Program& p = program_;
  p.id = g.glCreateProgram();
  g.glAttachShader(p.id, vs);
  g.glAttachShader(p.id, fs);
  g.glBindAttribLocation(p.id, 0, "a_position");
  g.glBindAttribLocation(p.id, 1, "a_texcoord");
  g.glLinkProgram(p.id);
  g.glDeleteShader(vs);
  g.glDeleteShader(fs);
  GLint linked = 0;
  g.glGetProgramiv(p.id, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[512] = {};
    g.glGetProgramInfoLog(p.id, sizeof log, nullptr, log);
    std::fprintf(stderr, "gesher GPU thread: program: %s\n", log);
    return false;
  }
  p.viewport = g.glGetUniformLocation(p.id, "u_viewport");
  p.texture = g.glGetUniformLocation(p.id, "u_texture");
  p.color = g.glGetUniformLocation(p.id, "u_color");
  p.alpha = g.glGetUniformLocation(p.id, "u_alpha");
  p.mode = g.glGetUniformLocation(p.id, "u_mode");
  p.opaque = g.glGetUniformLocation(p.id, "u_opaque");
  g.glGenBuffers(1, &p.vbo);
  // Float color buffers (RGBA16F buffers read back for the CPU).
  GLint n = 0;
  g.glGetIntegerv(GL_NUM_EXTENSIONS, &n);
  auto get_stringi = reinterpret_cast<const GLubyte*(GL_APIENTRY*)(GLenum, GLuint)>(
      gesher_egl_proc("glGetStringi"));
  for (GLint i = 0; get_stringi != nullptr && i < n; ++i) {
    const char* e = reinterpret_cast<const char*>(get_stringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
    if (e != nullptr && (std::strcmp(e, "GL_EXT_color_buffer_float") == 0 ||
                         std::strcmp(e, "GL_EXT_color_buffer_half_float") == 0)) {
      color_buffer_float_ = true;
    }
  }
  return true;
}

void Thread::Main() {
  bool ok = InitOnThread();
  {
    std::lock_guard<std::mutex> lock(queue_->mutex);
    queue_->id = std::this_thread::get_id();
    queue_->ok = ok;
    queue_->started = true;
  }
  queue_->cv.notify_all();
  if (!ok) return;
  Log("ANGLE ES 3 context ready (AHardwareBuffer textures, SurfaceControl composition)");
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(queue_->mutex);
      queue_->cv.wait(lock, [this] { return !queue_->tasks.empty(); });
      task = std::move(queue_->tasks.front());
      queue_->tasks.pop_front();
    }
    task();
  }
}

bool Thread::OnThread() const { return std::this_thread::get_id() == queue_->id; }

void Thread::Post(std::function<void()> fn) {
  if (OnThread()) {
    fn();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(queue_->mutex);
    queue_->tasks.push_back(std::move(fn));
  }
  queue_->cv.notify_all();
}

void Thread::Run(const std::function<void()>& fn) {
  if (OnThread()) {
    fn();
    return;
  }
  struct Done {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
  } done;
  {
    std::lock_guard<std::mutex> lock(queue_->mutex);
    queue_->tasks.push_back([&fn, &done] {
      fn();
      std::lock_guard<std::mutex> l(done.mutex);
      done.done = true;
      done.cv.notify_all();
    });
  }
  queue_->cv.notify_all();
  std::unique_lock<std::mutex> lock(done.mutex);
  done.cv.wait(lock, [&done] { return done.done; });
}

}  // namespace gesher::bionic::gpu
