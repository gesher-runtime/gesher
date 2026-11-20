// gesher: ASurfaceControl, ASurfaceTransaction and ASurfaceTransactionStats
// (libandroid.so, <android/surface_control.h>) for an app's ELF libraries
// (ADR 0089).
//
// Android composes a SurfaceControl's layers in SurfaceFlinger, on top of the
// window (a SurfaceView's surface) they were created from. Gesher has no
// compositor process: each window an ASurfaceControl tree hangs from is
// composed in the app's process, on its GPU thread (gpu_interop.h), and the
// result is posted to that ANativeWindow as its producer would post a frame
// (for a SurfaceView's surface in another process, through the forwarding
// Surface of surface_parcel.cc). So:
//  - a layer tree: ASurfaceControl_createFromWindow makes a layer attached to
//    the window, ASurfaceControl_create a child; reparent moves a layer (to
//    null: off the display); visibility hides a subtree; z orders siblings
//    (negative z below the parent's own content, as SurfaceFlinger draws);
//  - a layer's content: a buffer (an AHardwareBuffer, sampled through its
//    texture; premultiplied alpha), a color, both (the color behind the
//    buffer), placed by its buffer crop, transform and destination frame
//    (setGeometry, or setCrop/setPosition/setScale/setBufferTransform);
//    opaque layers are drawn without blending, plane alpha multiplies;
//  - ASurfaceTransaction_apply applies a transaction's changes atomically and
//    in order, snapshots every window whose tree changed and queues the
//    composition: each applied transaction is presented (none is dropped, so
//    back pressure holds), compositions in the order applied;
//  - callbacks run on a thread of their own, as Android's binder threads:
//    OnCommit once the transaction is applied (latched), OnComplete after the
//    composed frame was posted; ASurfaceTransactionStats carry the real latch
//    time (CLOCK_MONOTONIC, when composition latched the frame), the
//    acquire time of each new buffer (when its acquire fence was seen
//    signaled; -1 without a fence), no present fence (-1: there is no
//    display's present fence to give) and previous release fences of -1
//    (a replaced buffer is no longer read once the frame without it has been
//    composed, before OnComplete).
// Not modelled (accepted, without effect): damage regions (every frame is
// composed whole), data spaces and HDR metadata (composed as sRGB), frame
// rates, frame timelines. ASurfaceTransaction_setLuts is not provided.
#include "gpu_interop.h"  // first: the EGL/GL prototype macros
#include "hardware_buffer.h"
#include "surface_control.h"

#include "abi_types.h"

#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>

#include <android/data_space.h>
#include <android/hdr_metadata.h>
#include <android/native_window.h>

#include <errno.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// <system/window.h>'s values (the NDK's ANATIVEWINDOW_TRANSFORM_*).
#ifndef NATIVE_WINDOW_TRANSFORM_FLIP_H
#define NATIVE_WINDOW_TRANSFORM_FLIP_H 0x01
#define NATIVE_WINDOW_TRANSFORM_FLIP_V 0x02
#define NATIVE_WINDOW_TRANSFORM_ROT_90 0x04
#endif

// <android/surface_control.h>'s opaque types (the header's C++ prototypes are
// not needed here).
struct ASurfaceTransaction;
struct ASurfaceTransactionStats;

namespace gesher::bionic::sc {

namespace {

using ahb::Buffer;

constexpr uint32_t kLayerMagic = 0x4C435347;        // "GSCL"
constexpr uint32_t kTransactionMagic = 0x58545347;  // "GSTX"
constexpr int8_t kTransparent = 0, kTranslucent = 1, kOpaque = 2;

void Fatal(const char* what) {
  std::fprintf(stderr, "gesher SurfaceControl: %s\n", what);
  std::fflush(stderr);
  std::abort();
}

int64_t MonotonicNs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

using SysvCallback = void(BIONIC_SYSV*)(void* context, ASurfaceTransactionStats* stats);
using SysvBufferRelease = void(BIONIC_SYSV*)(void* context, int release_fence_fd);

struct Callback {
  SysvCallback fn = nullptr;
  void* context = nullptr;
};

struct ReleaseCallback {
  SysvBufferRelease fn = nullptr;
  void* context = nullptr;
};

// --- the callback thread (Android's binder threads) ----------------------------------------------

class CallbackThread {
 public:
  static CallbackThread& Get() {
    static CallbackThread* thread = new CallbackThread();
    return *thread;
  }
  void Post(std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push_back(std::move(fn));
    }
    cv_.notify_all();
  }

 private:
  CallbackThread() {
    std::thread([this] {
      EnterLoadedCode();  // the callbacks are the app's System V code
      for (;;) {
        std::function<void()> task;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          cv_.wait(lock, [this] { return !tasks_.empty(); });
          task = std::move(tasks_.front());
          tasks_.pop_front();
        }
        task();
      }
    }).detach();
  }
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
};

// --- layers --------------------------------------------------------------------------------------

struct Root;

struct Layer {
  uint32_t magic = kLayerMagic;
  int refs = 1;  // app references and one per tree link; under g_tree
  std::string name;
  int64_t java_object = 0;  // ForJavaSurfaceControl's key, or 0

  // The tree (under g_tree): a parent layer or a window holds a reference.
  Layer* parent = nullptr;
  Root* root = nullptr;  // the window it is attached to directly
  std::vector<Layer*> children;

  bool visible = true;
  int32_t z = 0;
  Buffer* buffer = nullptr;  // acquired
  int fence = -1;            // its acquire fence, until a composition takes it
  bool buffer_fenced = false;
  ReleaseCallback release;   // setBufferWithRelease's, for `buffer`
  bool has_color = false;
  float color[4] = {0, 0, 0, 0};  // premultiplied
  bool has_source = false;
  ARect source{0, 0, 0, 0};
  bool has_dest = false;
  ARect dest{0, 0, 0, 0};
  int32_t transform = 0;
  bool has_crop = false;
  ARect crop{0, 0, 0, 0};
  int32_t x = 0, y = 0;
  float scale_x = 1, scale_y = 1;
  int8_t transparency = kTranslucent;
  float alpha = 1;
  int32_t dataspace = ADATASPACE_UNKNOWN;
  bool back_pressure = false;
  float frame_rate = 0;
};

// A window with layers attached.
struct Root {
  ANativeWindow* window = nullptr;  // acquired while it has layers
  std::vector<Layer*> layers;
};

std::mutex g_tree;
std::unordered_map<ANativeWindow*, Root*>& Roots() {
  static auto* map = new std::unordered_map<ANativeWindow*, Root*>();
  return *map;
}
std::unordered_map<Layer*, bool>& Live() {
  static auto* map = new std::unordered_map<Layer*, bool>();
  return *map;
}
std::map<int64_t, Layer*>& JavaLayers() {
  static auto* map = new std::map<int64_t, Layer*>();
  return *map;
}

// What dropping references leaves to do outside g_tree.
struct Garbage {
  std::vector<Buffer*> buffers;
  std::vector<int> fences;
  std::vector<ReleaseCallback> releases;
  std::vector<ANativeWindow*> windows;

  void Run() {
    for (Buffer* b : buffers) ahb::Release(b);
    for (int fd : fences) close(fd);
    for (ANativeWindow* w : windows) ANativeWindow_release(w);
    if (!releases.empty()) {
      auto r = std::move(releases);
      CallbackThread::Get().Post([r] {
        for (const ReleaseCallback& c : r) c.fn(c.context, -1);
      });
    }
  }
};

Layer* LayerFromApp(const void* p) {
  auto* l = const_cast<Layer*>(static_cast<const Layer*>(p));
  return l != nullptr && Live().count(l) != 0 && l->magic == kLayerMagic ? l : nullptr;
}

void DropBufferLocked(Layer* l, Garbage* g) {
  if (l->buffer != nullptr) {
    g->buffers.push_back(l->buffer);
    if (l->release.fn != nullptr) g->releases.push_back(l->release);
  }
  if (l->fence >= 0) g->fences.push_back(l->fence);
  l->buffer = nullptr;
  l->fence = -1;
  l->release = ReleaseCallback();
}

void ReleaseLocked(Layer* l, Garbage* g);

// Takes `l` out of its parent or window (the link's reference is dropped).
void DetachLocked(Layer* l, Garbage* g) {
  if (l->parent != nullptr) {
    auto& c = l->parent->children;
    c.erase(std::remove(c.begin(), c.end(), l), c.end());
    l->parent = nullptr;
    ReleaseLocked(l, g);
  } else if (l->root != nullptr) {
    Root* r = l->root;
    r->layers.erase(std::remove(r->layers.begin(), r->layers.end(), l), r->layers.end());
    l->root = nullptr;
    if (r->layers.empty()) {
      Roots().erase(r->window);
      g->windows.push_back(r->window);
      delete r;
    }
    ReleaseLocked(l, g);
  }
}

void ReleaseLocked(Layer* l, Garbage* g) {
  if (--l->refs > 0) return;
  // No parent and no references: the layer and its links to its children go.
  Live().erase(l);
  if (l->java_object != 0) JavaLayers().erase(l->java_object);
  DropBufferLocked(l, g);
  std::vector<Layer*> children = std::move(l->children);
  for (Layer* c : children) {
    c->parent = nullptr;
    ReleaseLocked(c, g);
  }
  l->magic = 0;
  delete l;
}

void ReleaseLayer(Layer* l) {
  Garbage g;
  {
    std::lock_guard<std::mutex> lock(g_tree);
    ReleaseLocked(l, &g);
  }
  g.Run();
}

bool IsAncestorOrSelf(const Layer* maybe_ancestor, const Layer* l) {
  for (const Layer* p = l; p != nullptr; p = p->parent) {
    if (p == maybe_ancestor) return true;
  }
  return false;
}

Root* RootOf(Layer* l) {
  for (Layer* p = l; p != nullptr; p = p->parent) {
    if (p->parent == nullptr) return p->root;
  }
  return nullptr;
}

// --- snapshots: what a composition draws ----------------------------------------------------------

struct DrawOp {
  Buffer* buffer = nullptr;  // acquired; null for a color
  int fence = -1;            // owned
  const Layer* layer = nullptr;  // a key for the stats, never followed
  float quad[4][4] = {};     // TL, TR, BL, BR: x, y (target pixels, top-left origin), u, v
  int scissor[4] = {};       // x, y (top), width, height
  bool opaque = false;
  float alpha = 1;
  float color[4] = {};
};

struct Frame {
  ANativeWindow* window = nullptr;  // acquired
  std::vector<DrawOp> ops;
};

struct RectF {
  float l, t, r, b;
  bool empty() const { return r <= l || b <= t; }
  RectF intersect(const RectF& o) const {
    return RectF{std::max(l, o.l), std::max(t, o.t), std::min(r, o.r), std::min(b, o.b)};
  }
};

// x' = sx * x + tx (the NDK gives layers positions and scales only).
struct Affine {
  float sx = 1, sy = 1, tx = 0, ty = 0;
  Affine then_local(float x, float y, float scale_x, float scale_y) const {
    Affine m;
    m.tx = sx * x + tx;
    m.ty = sy * y + ty;
    m.sx = sx * scale_x;
    m.sy = sy * scale_y;
    return m;
  }
  RectF map(const RectF& r) const {
    float l = sx * r.l + tx, rr = sx * r.r + tx, t = sy * r.t + ty, b = sy * r.b + ty;
    return RectF{std::min(l, rr), std::min(t, b), std::max(l, rr), std::max(t, b)};
  }
};

RectF ToF(const ARect& r) {
  return RectF{static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
               static_cast<float>(r.bottom)};
}

void SetScissor(DrawOp* op, const RectF& clip) {
  const int l = static_cast<int>(std::floor(clip.l)), t = static_cast<int>(std::floor(clip.t));
  const int r = static_cast<int>(std::ceil(clip.r)), b = static_cast<int>(std::ceil(clip.b));
  op->scissor[0] = l;
  op->scissor[1] = t;
  op->scissor[2] = std::max(0, r - l);
  op->scissor[3] = std::max(0, b - t);
}

// The layer's own content, in local coordinates (empty without a buffer).
bool BufferRect(const Layer* l, ARect* src, RectF* dst) {
  if (l->buffer == nullptr) return false;
  const AHardwareBuffer_Desc& d = ahb::Desc(l->buffer);
  ARect s{0, 0, static_cast<int32_t>(d.width), static_cast<int32_t>(d.height)};
  if (l->has_source) {
    s.left = std::max(0, l->source.left);
    s.top = std::max(0, l->source.top);
    s.right = std::min(static_cast<int32_t>(d.width), l->source.right);
    s.bottom = std::min(static_cast<int32_t>(d.height), l->source.bottom);
  }
  if (s.right <= s.left || s.bottom <= s.top) return false;
  const bool rot = (l->transform & NATIVE_WINDOW_TRANSFORM_ROT_90) != 0;
  const float cw = static_cast<float>(rot ? s.bottom - s.top : s.right - s.left);
  const float ch = static_cast<float>(rot ? s.right - s.left : s.bottom - s.top);
  *src = s;
  *dst = l->has_dest ? ToF(l->dest) : RectF{0, 0, cw, ch};
  return !dst->empty();
}

void SnapshotLayer(Layer* l, const Affine& parent, const RectF& clip, std::vector<DrawOp>* ops);

void SnapshotChildren(Layer* l, const Affine& m, const RectF& clip, bool below,
                      std::vector<DrawOp>* ops) {
  std::vector<Layer*> sorted = l->children;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const Layer* a, const Layer* b) { return a->z < b->z; });
  for (Layer* c : sorted) {
    if ((c->z < 0) == below) SnapshotLayer(c, m, clip, ops);
  }
}

void SnapshotLayer(Layer* l, const Affine& parent, const RectF& clip, std::vector<DrawOp>* ops) {
  if (!l->visible) return;
  const Affine m = parent.then_local(static_cast<float>(l->x), static_cast<float>(l->y), l->scale_x,
                                     l->scale_y);
  RectF bounds = clip;
  if (l->has_crop && l->crop.right > l->crop.left && l->crop.bottom > l->crop.top) {
    bounds = bounds.intersect(m.map(ToF(l->crop)));
  }
  if (bounds.empty()) return;
  SnapshotChildren(l, m, bounds, /*below=*/true, ops);

  ARect src{};
  RectF dst_local{};
  const bool has_buffer = BufferRect(l, &src, &dst_local);
  if (l->has_color && l->color[3] > 0) {
    // The color fills the layer's bounds: its crop, else its buffer, else its parent's.
    RectF fill = bounds;
    if (!(l->has_crop && l->crop.right > l->crop.left) && has_buffer) {
      fill = fill.intersect(m.map(dst_local));
    }
    if (!fill.empty()) {
      DrawOp op;
      op.layer = l;
      const float q[4][2] = {{fill.l, fill.t}, {fill.r, fill.t}, {fill.l, fill.b}, {fill.r, fill.b}};
      for (int i = 0; i < 4; ++i) {
        op.quad[i][0] = q[i][0];
        op.quad[i][1] = q[i][1];
      }
      SetScissor(&op, fill);
      std::memcpy(op.color, l->color, sizeof op.color);
      op.alpha = l->alpha;
      ops->push_back(op);
    }
  }
  if (has_buffer) {
    const RectF dst = m.map(dst_local);
    const RectF visible = dst.intersect(bounds);
    if (!visible.empty()) {
      const AHardwareBuffer_Desc& d = ahb::Desc(l->buffer);
      DrawOp op;
      op.layer = l;
      op.buffer = l->buffer;
      ahb::Acquire(op.buffer);
      op.fence = l->fence;  // the first composition with the buffer waits for it
      l->fence = -1;
      const float corners[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
      for (int i = 0; i < 4; ++i) {
        const float dx = corners[i][0], dy = corners[i][1];
        op.quad[i][0] = dst.l + dx * (dst.r - dst.l);
        op.quad[i][1] = dst.t + dy * (dst.b - dst.t);
        // Display point -> source point: undo the rotation, then the flips.
        float px = dx, py = dy;
        if ((l->transform & NATIVE_WINDOW_TRANSFORM_ROT_90) != 0) {
          px = dy;
          py = 1 - dx;
        }
        if ((l->transform & NATIVE_WINDOW_TRANSFORM_FLIP_H) != 0) px = 1 - px;
        if ((l->transform & NATIVE_WINDOW_TRANSFORM_FLIP_V) != 0) py = 1 - py;
        op.quad[i][2] = (static_cast<float>(src.left) + px * static_cast<float>(src.right - src.left)) /
                        static_cast<float>(d.width);
        op.quad[i][3] = (static_cast<float>(src.top) + py * static_cast<float>(src.bottom - src.top)) /
                        static_cast<float>(d.height);
      }
      SetScissor(&op, visible);
      op.opaque = l->transparency == kOpaque;
      op.alpha = l->transparency == kTransparent ? 0.0f : l->alpha;
      ops->push_back(op);
    }
  }
  SnapshotChildren(l, m, bounds, /*below=*/false, ops);
}

Frame SnapshotRoot(Root* r) {
  Frame f;
  f.window = r->window;
  ANativeWindow_acquire(f.window);
  const float w = static_cast<float>(std::max(1, ANativeWindow_getWidth(r->window)));
  const float h = static_cast<float>(std::max(1, ANativeWindow_getHeight(r->window)));
  std::vector<Layer*> sorted = r->layers;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const Layer* a, const Layer* b) { return a->z < b->z; });
  for (Layer* l : sorted) SnapshotLayer(l, Affine(), RectF{0, 0, w, h}, &f.ops);
  return f;
}

// --- transactions --------------------------------------------------------------------------------

enum Field : uint32_t {
  kVisibility = 1u << 0,
  kZ = 1u << 1,
  kBuffer = 1u << 2,
  kColor = 1u << 3,
  kSource = 1u << 4,
  kDest = 1u << 5,
  kTransform = 1u << 6,
  kCrop = 1u << 7,
  kPosition = 1u << 8,
  kScale = 1u << 9,
  kTransparency = 1u << 10,
  kAlpha = 1u << 11,
  kDataspace = 1u << 12,
  kReparent = 1u << 13,
  kBackPressure = 1u << 14,
  kFrameRate = 1u << 15,
};

struct Change {
  Layer* layer = nullptr;  // acquired by the transaction
  uint32_t fields = 0;
  bool visible = true;
  int32_t z = 0;
  Buffer* buffer = nullptr;  // acquired
  int fence = -1;
  ReleaseCallback release;
  float color[4] = {};
  bool has_source = false;
  ARect source{};
  ARect dest{};
  int32_t transform = 0;
  bool has_crop = false;
  ARect crop{};
  int32_t x = 0, y = 0;
  float scale_x = 1, scale_y = 1;
  int8_t transparency = kTranslucent;
  float alpha = 1;
  int32_t dataspace = 0;
  Layer* new_parent = nullptr;  // acquired
  bool back_pressure = false;
  float frame_rate = 0;
};

struct Transaction {
  uint32_t magic = kTransactionMagic;
  std::mutex mutex;
  std::vector<Change> changes;
  std::vector<Callback> on_complete, on_commit;
  int64_t desired_present_time = 0;
  int64_t vsync_id = 0;
};

std::mutex g_transactions_mutex;
std::unordered_map<Transaction*, bool>& LiveTransactions() {
  static auto* map = new std::unordered_map<Transaction*, bool>();
  return *map;
}

Transaction* TransactionFromApp(void* p) {
  auto* t = static_cast<Transaction*>(p);
  std::lock_guard<std::mutex> lock(g_transactions_mutex);
  return t != nullptr && LiveTransactions().count(t) != 0 && t->magic == kTransactionMagic ? t : nullptr;
}

// The change for `layer` in `t` (t->mutex held); null for an invalid layer.
Change* ChangeFor(Transaction* t, ASurfaceControl* surface_control) {
  Layer* layer;
  {
    std::lock_guard<std::mutex> lock(g_tree);
    layer = LayerFromApp(surface_control);
    if (layer == nullptr) return nullptr;
    for (Change& c : t->changes) {
      if (c.layer == layer) return &c;
    }
    layer->refs++;
  }
  t->changes.emplace_back();
  t->changes.back().layer = layer;
  return &t->changes.back();
}

// Drops what a change holds (it was not applied, or was replaced).
void DropChange(Change* c, Garbage* g) {
  if (c->buffer != nullptr) {
    g->buffers.push_back(c->buffer);
    if (c->release.fn != nullptr) g->releases.push_back(c->release);
  }
  if (c->fence >= 0) g->fences.push_back(c->fence);
  c->buffer = nullptr;
  c->fence = -1;
  c->release = ReleaseCallback();
}

struct Stats {
  int64_t latch_time = 0;
  bool completed = false;  // false: the OnCommit callback's
  struct Surface {
    Layer* layer;
    int64_t acquire_time;
  };
  std::vector<Surface> surfaces;
};

// What a composition does for one applied transaction.
struct Job {
  std::vector<Frame> frames;
  std::vector<Layer*> touched;     // acquired: the stats' surfaces
  std::vector<Layer*> new_buffer;  // layers given a buffer (keys only)
  Garbage replaced;                // buffers replaced: released after composition
  std::vector<Callback> on_complete;
  int64_t desired_present_time = 0;
  std::map<const Layer*, int64_t> acquire_times;
  std::vector<ANativeWindow*> forget;  // windows without layers now (keys only)
};

// --- composition (GPU thread) --------------------------------------------------------------------

struct Target {
  GLuint fbo = 0, texture = 0;
  int width = 0, height = 0;
};

std::unordered_map<ANativeWindow*, Target>& Targets() {  // GPU thread only
  static auto* map = new std::unordered_map<ANativeWindow*, Target>();
  return *map;
}

// Drops the GPU thread's target of a window that has no layers any more.
void ForgetTarget(gpu::Thread* thread, ANativeWindow* window) {
  auto it = Targets().find(window);
  if (it == Targets().end()) return;
  const gpu::Gl& g = thread->gl();
  g.glDeleteFramebuffers(1, &it->second.fbo);
  g.glDeleteTextures(1, &it->second.texture);
  Targets().erase(it);
}

int64_t WaitFence(int fd) {
  pollfd p{fd, POLLIN, 0};
  for (;;) {
    int n = poll(&p, 1, 1000);
    if (n > 0) break;
    if (n < 0 && errno != EINTR) break;
    if (n == 0) {
      std::fprintf(stderr, "gesher SurfaceControl: still waiting for a buffer's acquire fence\n");
      std::fflush(stderr);
    }
  }
  close(fd);
  return MonotonicNs();
}

void Compose(gpu::Thread* thread, Frame* f, Job* job) {
  const gpu::Gl& g = thread->gl();
  ANativeWindow* window = f->window;
  const int w = std::max(1, ANativeWindow_getWidth(window));
  const int h = std::max(1, ANativeWindow_getHeight(window));
  Target& t = Targets()[window];
  if (t.width != w || t.height != h || t.fbo == 0) {
    if (t.fbo != 0) g.glDeleteFramebuffers(1, &t.fbo);
    if (t.texture != 0) g.glDeleteTextures(1, &t.texture);
    g.glGenTextures(1, &t.texture);
    g.glBindTexture(GL_TEXTURE_2D, t.texture);
    g.glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    g.glGenFramebuffers(1, &t.fbo);
    g.glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    g.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.texture, 0);
    t.width = w;
    t.height = h;
  }
  g.glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
  g.glViewport(0, 0, w, h);
  g.glDisable(GL_SCISSOR_TEST);
  g.glClearColor(0, 0, 0, 0);
  g.glClear(GL_COLOR_BUFFER_BIT);
  const gpu::Thread::Program& p = thread->program();
  g.glUseProgram(p.id);
  g.glUniform2f(p.viewport, static_cast<float>(w), static_cast<float>(h));
  g.glUniform1i(p.texture, 0);
  g.glActiveTexture(GL_TEXTURE0);
  g.glBindBuffer(GL_ARRAY_BUFFER, p.vbo);
  g.glEnableVertexAttribArray(0);
  g.glEnableVertexAttribArray(1);
  g.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
  g.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<const void*>(2 * sizeof(float)));
  g.glEnable(GL_SCISSOR_TEST);
  for (DrawOp& op : f->ops) {
    if (op.fence >= 0) {
      job->acquire_times[op.layer] = WaitFence(op.fence);
      op.fence = -1;
    }
    if (op.scissor[2] <= 0 || op.scissor[3] <= 0) continue;
    GLuint texture = 0;
    if (op.buffer != nullptr) {
      texture = ahb::CompositorTexture(op.buffer);
      if (texture == 0) continue;  // no GPU form (a BLOB): nothing to show
      g.glBindVertexArray(0);
      g.glUseProgram(p.id);
      g.glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
      g.glBindBuffer(GL_ARRAY_BUFFER, p.vbo);
      g.glBindTexture(GL_TEXTURE_2D, texture);
      g.glUniform1f(p.mode, 0);
    } else {
      g.glUniform1f(p.mode, 1);
      g.glUniform4f(p.color, op.color[0], op.color[1], op.color[2], op.color[3]);
    }
    g.glUniform1f(p.alpha, op.alpha);
    g.glUniform1f(p.opaque, op.opaque ? 1.0f : 0.0f);
    if (op.opaque && op.alpha >= 1.0f) {
      g.glDisable(GL_BLEND);
    } else {
      g.glEnable(GL_BLEND);
      g.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied
    }
    g.glScissor(op.scissor[0], op.scissor[1], op.scissor[2], op.scissor[3]);
    g.glBufferData(GL_ARRAY_BUFFER, sizeof op.quad, op.quad, GL_STREAM_DRAW);
    g.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  g.glDisable(GL_SCISSOR_TEST);
  g.glDisable(GL_BLEND);

  // The frame to the window, as its producer: RGBA rows, top first.
  ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
  ANativeWindow_Buffer out{};
  if (ANativeWindow_lock(window, &out, nullptr) != 0) {
    static std::atomic<bool> said{false};
    if (!said.exchange(true)) {
      std::fprintf(stderr, "gesher SurfaceControl: a window took no frame (lock failed)\n");
      std::fflush(stderr);
    }
    return;
  }
  const int rw = std::min(w, out.width), rh = std::min(h, out.height);
  if (out.bits != nullptr && rw > 0 && rh > 0) {
    g.glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (out.format == WINDOW_FORMAT_RGBA_8888 || out.format == WINDOW_FORMAT_RGBX_8888) {
      g.glPixelStorei(GL_PACK_ROW_LENGTH, out.stride);
      g.glReadPixels(0, 0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, out.bits);
      g.glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    } else {
      // BGRA (5) or 16-bit windows: converted from an RGBA read-back.
      std::vector<uint8_t> px(static_cast<size_t>(rw) * rh * 4);
      g.glReadPixels(0, 0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      for (int y = 0; y < rh; ++y) {
        const uint8_t* s = &px[static_cast<size_t>(y) * rw * 4];
        if (out.format == WINDOW_FORMAT_RGB_565) {
          auto* d = static_cast<uint16_t*>(out.bits) + static_cast<size_t>(y) * out.stride;
          for (int x = 0; x < rw; ++x) {
            d[x] = static_cast<uint16_t>((s[x * 4] >> 3) << 11 | (s[x * 4 + 1] >> 2) << 5 |
                                         (s[x * 4 + 2] >> 3));
          }
        } else {
          auto* d = static_cast<uint8_t*>(out.bits) + static_cast<size_t>(y) * out.stride * 4;
          for (int x = 0; x < rw; ++x) {
            d[x * 4 + 0] = s[x * 4 + 2];
            d[x * 4 + 1] = s[x * 4 + 1];
            d[x * 4 + 2] = s[x * 4 + 0];
            d[x * 4 + 3] = s[x * 4 + 3];
          }
        }
      }
    }
  }
  g.glBindFramebuffer(GL_FRAMEBUFFER, 0);
  ANativeWindow_unlockAndPost(window);
}

void RunJob(gpu::Thread* thread, Job* job) {
  if (job->desired_present_time > 0) {
    // "At or after the time specified": wait for it (at most a second).
    const int64_t wait = job->desired_present_time - MonotonicNs();
    if (wait > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<int64_t>(wait, 1000000000)));
  }
  const int64_t latch = MonotonicNs();
  for (Frame& f : job->frames) {
    Compose(thread, &f, job);
    for (DrawOp& op : f.ops) {
      if (op.fence >= 0) close(op.fence);
      if (op.buffer != nullptr) ahb::Release(op.buffer);
    }
    ANativeWindow_release(f.window);
  }
  job->frames.clear();
  if (thread != nullptr) {
    for (ANativeWindow* w : job->forget) ForgetTarget(thread, w);
  }
  // Buffers replaced by this transaction were last read by an earlier
  // composition: released now (with their release callbacks), before OnComplete.
  job->replaced.Run();

  auto* stats = new Stats();
  stats->latch_time = latch;
  stats->completed = true;
  for (Layer* l : job->touched) {
    int64_t acquire = -1;
    auto it = job->acquire_times.find(l);
    if (it != job->acquire_times.end()) acquire = it->second;
    stats->surfaces.push_back({l, acquire});
  }
  std::vector<Callback> callbacks = std::move(job->on_complete);
  std::vector<Layer*> touched = std::move(job->touched);
  delete job;
  CallbackThread::Get().Post([stats, callbacks, touched] {
    for (const Callback& c : callbacks) c.fn(c.context, reinterpret_cast<ASurfaceTransactionStats*>(stats));
    delete stats;
    for (Layer* l : touched) ReleaseLayer(l);
  });
}

void Apply(Transaction* t) {
  gpu::Thread* thread = gpu::Thread::Get();
  auto* job = new Job();
  std::vector<Callback> on_commit;
  int64_t latch = 0;
  Garbage garbage;
  std::vector<ANativeWindow*> forgotten;
  {
    std::lock_guard<std::mutex> tx_lock(t->mutex);
    std::lock_guard<std::mutex> lock(g_tree);
    std::vector<Root*> roots;
    auto note_root = [&roots](Root* r) {
      if (r != nullptr && std::find(roots.begin(), roots.end(), r) == roots.end()) roots.push_back(r);
    };
    for (Change& c : t->changes) {
      Layer* l = c.layer;
      note_root(RootOf(l));
      if ((c.fields & kReparent) != 0) {
        Layer* np = c.new_parent;
        // A layer cannot become its own descendant.
        if (np == nullptr || !IsAncestorOrSelf(l, np)) {
          l->refs++;  // held across the move
          Root* old_root = RootOf(l);
          ANativeWindow* old_window = old_root != nullptr ? old_root->window : nullptr;
          DetachLocked(l, &garbage);
          if (old_window != nullptr && Roots().count(old_window) == 0) {
            // The window lost its last layer (its Root is gone): it gets an
            // empty frame below.
            roots.erase(std::remove(roots.begin(), roots.end(), old_root), roots.end());
          }
          if (np != nullptr) {
            np->children.push_back(l);
            l->parent = np;
            l->refs++;
          }
          ReleaseLocked(l, &garbage);
        }
      }
      if (c.fields & kVisibility) l->visible = c.visible;
      if (c.fields & kZ) l->z = c.z;
      if (c.fields & kBuffer) {
        DropBufferLocked(l, &job->replaced);
        l->buffer = c.buffer;
        l->fence = c.fence;
        l->buffer_fenced = c.fence >= 0;
        l->release = c.release;
        c.buffer = nullptr;
        c.fence = -1;
        c.release = ReleaseCallback();
        if (l->buffer != nullptr) job->new_buffer.push_back(l);
      }
      if (c.fields & kColor) {
        l->has_color = true;
        std::memcpy(l->color, c.color, sizeof l->color);
      }
      if (c.fields & kSource) {
        l->has_source = c.has_source;
        l->source = c.source;
      }
      if (c.fields & kDest) {
        l->has_dest = true;
        l->dest = c.dest;
      }
      if (c.fields & kTransform) l->transform = c.transform;
      if (c.fields & kCrop) {
        l->has_crop = c.has_crop;
        l->crop = c.crop;
      }
      if (c.fields & kPosition) {
        l->x = c.x;
        l->y = c.y;
      }
      if (c.fields & kScale) {
        l->scale_x = c.scale_x;
        l->scale_y = c.scale_y;
      }
      if (c.fields & kTransparency) l->transparency = c.transparency;
      if (c.fields & kAlpha) l->alpha = c.alpha;
      if (c.fields & kDataspace) l->dataspace = c.dataspace;
      if (c.fields & kBackPressure) l->back_pressure = c.back_pressure;
      if (c.fields & kFrameRate) l->frame_rate = c.frame_rate;
      note_root(RootOf(l));
      job->touched.push_back(l);  // the change's reference moves to the job
      if (c.new_parent != nullptr) ReleaseLocked(c.new_parent, &garbage);
    }
    t->changes.clear();
    for (Root* r : roots) job->frames.push_back(SnapshotRoot(r));
    latch = MonotonicNs();
    for (ANativeWindow* w : garbage.windows) forgotten.push_back(w);
    job->on_complete = std::move(t->on_complete);
    on_commit = std::move(t->on_commit);
    job->desired_present_time = t->desired_present_time;
    t->on_complete.clear();
    t->on_commit.clear();
    t->desired_present_time = 0;
  }
  // A window whose last layer left still gets an (empty) frame: its old
  // layers are no longer shown.
  for (ANativeWindow* w : forgotten) {
    Frame f;
    f.window = w;
    ANativeWindow_acquire(w);
    job->frames.push_back(std::move(f));
    job->forget.push_back(w);
  }
  garbage.Run();
  if (!on_commit.empty()) {
    auto* stats = new Stats();
    stats->latch_time = latch;
    stats->completed = false;
    std::vector<Layer*> held;
    {
      std::lock_guard<std::mutex> lock(g_tree);
      for (Layer* l : job->touched) {
        l->refs++;
        held.push_back(l);
        stats->surfaces.push_back({l, -1});
      }
    }
    CallbackThread::Get().Post([stats, on_commit, held] {
      for (const Callback& c : on_commit) c.fn(c.context, reinterpret_cast<ASurfaceTransactionStats*>(stats));
      delete stats;
      for (Layer* l : held) ReleaseLayer(l);
    });
  }
  if (thread == nullptr) {
    // No GPU thread: nothing can be composed (createFromWindow refused the
    // windows), but callbacks still complete in order.
    for (Frame& f : job->frames) {
      for (DrawOp& op : f.ops) {
        if (op.fence >= 0) close(op.fence);
        if (op.buffer != nullptr) ahb::Release(op.buffer);
      }
      ANativeWindow_release(f.window);
    }
    job->frames.clear();
    CallbackThread::Get().Post([job] { RunJob(nullptr, job); });
    return;
  }
  thread->Post([thread, job] { RunJob(thread, job); });
}

// --- the NDK functions ---------------------------------------------------------------------------

Layer* NewLayer(const char* name) {
  auto* l = new Layer();
  l->name = name != nullptr ? name : "";
  Live()[l] = true;
  return l;
}

ASurfaceControl* BIONIC_SYSV CreateFromWindow(ANativeWindow* window, const char* debug_name) {
  if (window == nullptr || debug_name == nullptr) Fatal("ASurfaceControl_createFromWindow: null argument");
  if (gpu::Thread::Get() == nullptr) {
    std::fprintf(stderr, "gesher SurfaceControl: no GPU thread to compose \"%s\" (no ANGLE device)\n",
                 debug_name);
    std::fflush(stderr);
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(g_tree);
  Root*& r = Roots()[window];
  if (r == nullptr) {
    r = new Root();
    r->window = window;
    ANativeWindow_acquire(window);
  }
  Layer* l = NewLayer(debug_name);
  l->refs = 2;  // the app's and the window's
  l->root = r;
  r->layers.push_back(l);
  return reinterpret_cast<ASurfaceControl*>(l);
}

ASurfaceControl* BIONIC_SYSV Create(ASurfaceControl* parent, const char* debug_name) {
  if (parent == nullptr || debug_name == nullptr) Fatal("ASurfaceControl_create: null argument");
  std::lock_guard<std::mutex> lock(g_tree);
  Layer* p = LayerFromApp(parent);
  if (p == nullptr) return nullptr;
  Layer* l = NewLayer(debug_name);
  l->refs = 2;  // the app's and the parent's
  l->parent = p;
  p->children.push_back(l);
  return reinterpret_cast<ASurfaceControl*>(l);
}

void BIONIC_SYSV AcquireControl(ASurfaceControl* surface_control) {
  std::lock_guard<std::mutex> lock(g_tree);
  if (Layer* l = LayerFromApp(surface_control)) l->refs++;
}

void BIONIC_SYSV ReleaseControl(ASurfaceControl* surface_control) {
  Garbage g;
  {
    std::lock_guard<std::mutex> lock(g_tree);
    if (Layer* l = LayerFromApp(surface_control)) ReleaseLocked(l, &g);
  }
  g.Run();
}

ASurfaceTransaction* BIONIC_SYSV TransactionCreate() {
  auto* t = new Transaction();
  std::lock_guard<std::mutex> lock(g_transactions_mutex);
  LiveTransactions()[t] = true;
  return reinterpret_cast<ASurfaceTransaction*>(t);
}

void BIONIC_SYSV TransactionDelete(ASurfaceTransaction* transaction) {
  Transaction* t = TransactionFromApp(transaction);
  if (t == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(g_transactions_mutex);
    LiveTransactions().erase(t);
  }
  Garbage g;
  {
    std::lock_guard<std::mutex> lock(g_tree);
    for (Change& c : t->changes) {
      DropChange(&c, &g);
      if (c.new_parent != nullptr) ReleaseLocked(c.new_parent, &g);
      ReleaseLocked(c.layer, &g);
    }
  }
  t->magic = 0;
  delete t;
  g.Run();
}

void BIONIC_SYSV TransactionApply(ASurfaceTransaction* transaction) {
  if (Transaction* t = TransactionFromApp(transaction)) Apply(t);
}

// Runs `edit` on the change for (transaction, surface_control).
template <typename F>
void Edit(ASurfaceTransaction* transaction, ASurfaceControl* surface_control, F edit) {
  Transaction* t = TransactionFromApp(transaction);
  if (t == nullptr) return;
  std::lock_guard<std::mutex> lock(t->mutex);
  Change* c = ChangeFor(t, surface_control);
  if (c != nullptr) edit(c);
}

void BIONIC_SYSV SetOnComplete(ASurfaceTransaction* transaction, void* context, SysvCallback fn) {
  Transaction* t = TransactionFromApp(transaction);
  if (t == nullptr || fn == nullptr) return;
  std::lock_guard<std::mutex> lock(t->mutex);
  t->on_complete.push_back({fn, context});
}

void BIONIC_SYSV SetOnCommit(ASurfaceTransaction* transaction, void* context, SysvCallback fn) {
  Transaction* t = TransactionFromApp(transaction);
  if (t == nullptr || fn == nullptr) return;
  std::lock_guard<std::mutex> lock(t->mutex);
  t->on_commit.push_back({fn, context});
}

void BIONIC_SYSV Reparent(ASurfaceTransaction* transaction, ASurfaceControl* surface_control,
                          ASurfaceControl* new_parent) {
  Garbage g;
  Edit(transaction, surface_control, [&](Change* c) {
    std::lock_guard<std::mutex> lock(g_tree);
    Layer* np = new_parent != nullptr ? LayerFromApp(new_parent) : nullptr;
    if (new_parent != nullptr && np == nullptr) return;
    if (c->new_parent != nullptr) ReleaseLocked(c->new_parent, &g);
    if (np != nullptr) np->refs++;
    c->new_parent = np;
    c->fields |= kReparent;
  });
  g.Run();
}

void BIONIC_SYSV SetVisibility(ASurfaceTransaction* transaction, ASurfaceControl* surface_control,
                               int8_t visibility) {
  Edit(transaction, surface_control, [&](Change* c) {
    c->visible = visibility != 0;
    c->fields |= kVisibility;
  });
}

void BIONIC_SYSV SetZOrder(ASurfaceTransaction* transaction, ASurfaceControl* surface_control,
                           int32_t z_order) {
  Edit(transaction, surface_control, [&](Change* c) {
    c->z = z_order;
    c->fields |= kZ;
  });
}

void SetBufferImpl(ASurfaceTransaction* transaction, ASurfaceControl* surface_control,
                   AHardwareBuffer* buffer, int acquire_fence_fd, ReleaseCallback release) {
  Buffer* b = buffer != nullptr ? ahb::FromApp(buffer) : nullptr;
  bool taken = false;
  Garbage g;
  Edit(transaction, surface_control, [&](Change* c) {
    // A buffer this transaction already had is replaced: released now.
    DropChange(c, &g);
    if (b != nullptr) {
      ahb::Acquire(b);
      ahb::NoteSubmitted(b);
    }
    c->buffer = b;
    c->fence = acquire_fence_fd;
    c->release = b != nullptr ? release : ReleaseCallback();
    c->fields |= kBuffer;
    taken = true;
  });
  if (!taken && acquire_fence_fd >= 0) close(acquire_fence_fd);  // the framework owns it
  g.Run();
}

void BIONIC_SYSV SetBuffer(ASurfaceTransaction* transaction, ASurfaceControl* surface_control,
                           AHardwareBuffer* buffer, int acquire_fence_fd) {
  SetBufferImpl(transaction, surface_control, buffer, acquire_fence_fd, ReleaseCallback());
}

void BIONIC_SYSV SetBufferWithRelease(ASurfaceTransaction* transaction,
                                      ASurfaceControl* surface_control, AHardwareBuffer* buffer,
                                      int acquire_fence_fd, void* context, SysvBufferRelease fn) {
  SetBufferImpl(transaction, surface_control, buffer, acquire_fence_fd, ReleaseCallback{fn, context});
}

void BIONIC_SYSV SetColor(ASurfaceTransaction* transaction, ASurfaceControl* surface_control,
                          float r, float g, float b, float alpha, int32_t /*dataspace*/) {
  Edit(transaction, surface_control, [&](Change* c) {
    auto clamp = [](float v) { return std::min(1.0f, std::max(0.0f, v)); };
    const float a = clamp(alpha);
    c->color[0] = clamp(r) * a;
    c->color[1] = clamp(g) * a;
    c->color[2] = clamp(b) * a;
    c->color[3] = a;
    c->fields |= kColor;
  });
}

void BIONIC_SYSV SetGeometry(ASurfaceTransaction* transaction, ASurfaceControl* surface_control,
                             const ARect* source, const ARect* destination, int32_t transform) {
  if (source == nullptr || destination == nullptr) Fatal("ASurfaceTransaction_setGeometry: null rect");
  Edit(transaction, surface_control, [&](Change* c) {
    // As Android's: the source's top-left clamped to the buffer, an invalid
    // source means the whole buffer.
    ARect s = *source;
    s.left = std::max(s.left, 0);
    s.top = std::max(s.top, 0);
    c->has_source = s.right > s.left && s.bottom > s.top;
    c->source = s;
    c->dest = *destination;
    c->transform = transform & (NATIVE_WINDOW_TRANSFORM_FLIP_H | NATIVE_WINDOW_TRANSFORM_FLIP_V |
                                NATIVE_WINDOW_TRANSFORM_ROT_90);
    c->fields |= kSource | kDest | kTransform;
  });
}

void BIONIC_SYSV SetCrop(ASurfaceTransaction* transaction, ASurfaceControl* surface_control,
                         const ARect* crop) {
  if (crop == nullptr) Fatal("ASurfaceTransaction_setCrop: null rect");
  Edit(transaction, surface_control, [&](Change* c) {
    c->has_crop = crop->right > crop->left && crop->bottom > crop->top;
    c->crop = *crop;
    c->fields |= kCrop;
  });
}

void BIONIC_SYSV SetPosition(ASurfaceTransaction* transaction, ASurfaceControl* surface_control,
                             int32_t x, int32_t y) {
  Edit(transaction, surface_control, [&](Change* c) {
    c->x = x;
    c->y = y;
    c->fields |= kPosition;
  });
}

void BIONIC_SYSV SetBufferTransform(ASurfaceTransaction* transaction,
                                    ASurfaceControl* surface_control, int32_t transform) {
  Edit(transaction, surface_control, [&](Change* c) {
    c->transform = transform & (NATIVE_WINDOW_TRANSFORM_FLIP_H | NATIVE_WINDOW_TRANSFORM_FLIP_V |
                                NATIVE_WINDOW_TRANSFORM_ROT_90);
    c->fields |= kTransform;
  });
}

void BIONIC_SYSV SetScale(ASurfaceTransaction* transaction, ASurfaceControl* surface_control,
                          float x_scale, float y_scale) {
  if (!(x_scale > 0) || !(y_scale > 0)) Fatal("ASurfaceTransaction_setScale: scale must be > 0");
  Edit(transaction, surface_control, [&](Change* c) {
    c->scale_x = x_scale;
    c->scale_y = y_scale;
    c->fields |= kScale;
  });
}

void BIONIC_SYSV SetBufferTransparency(ASurfaceTransaction* transaction,
                                       ASurfaceControl* surface_control, int8_t transparency) {
  Edit(transaction, surface_control, [&](Change* c) {
    c->transparency = transparency;
    c->fields |= kTransparency;
  });
}

void BIONIC_SYSV SetDamageRegion(ASurfaceTransaction* transaction, ASurfaceControl* surface_control,
                                 const ARect* /*rects*/, uint32_t /*count*/) {
  // Every frame is composed whole: the damage is a hint with no effect here.
  Edit(transaction, surface_control, [](Change*) {});
}

void BIONIC_SYSV SetDesiredPresentTime(ASurfaceTransaction* transaction, int64_t time) {
  Transaction* t = TransactionFromApp(transaction);
  if (t == nullptr) return;
  std::lock_guard<std::mutex> lock(t->mutex);
  t->desired_present_time = time;
}

void BIONIC_SYSV SetBufferAlpha(ASurfaceTransaction* transaction, ASurfaceControl* surface_control,
                                float alpha) {
  if (!(alpha >= 0.0f && alpha <= 1.0f)) Fatal("ASurfaceTransaction_setBufferAlpha: alpha not in [0, 1]");
  Edit(transaction, surface_control, [&](Change* c) {
    c->alpha = alpha;
    c->fields |= kAlpha;
  });
}

void BIONIC_SYSV SetBufferDataSpace(ASurfaceTransaction* transaction,
                                    ASurfaceControl* surface_control, int32_t data_space) {
  Edit(transaction, surface_control, [&](Change* c) {
    c->dataspace = data_space;
    c->fields |= kDataspace;
  });
}

void BIONIC_SYSV SetHdrMetadataSmpte2086(ASurfaceTransaction* transaction,
                                         ASurfaceControl* surface_control, const void* /*metadata*/) {
  Edit(transaction, surface_control, [](Change*) {});  // composed as SDR sRGB
}

void BIONIC_SYSV SetHdrMetadataCta8613(ASurfaceTransaction* transaction,
                                       ASurfaceControl* surface_control, const void* /*metadata*/) {
  Edit(transaction, surface_control, [](Change*) {});  // composed as SDR sRGB
}

void BIONIC_SYSV SetExtendedRangeBrightness(ASurfaceTransaction* transaction,
                                            ASurfaceControl* surface_control, float current_ratio,
                                            float desired_ratio) {
  if (!std::isfinite(current_ratio) || current_ratio < 1.0f) {
    Fatal("setExtendedRangeBrightness: currentBufferRatio isn't finite or >= 1.0f");
  }
  if (!std::isfinite(desired_ratio) || desired_ratio < 1.0f) {
    Fatal("setExtendedRangeBrightness: desiredRatio isn't finite or >= 1.0f");
  }
  Edit(transaction, surface_control, [](Change*) {});  // an SDR display: no headroom to give
}

void BIONIC_SYSV SetDesiredHdrHeadroom(ASurfaceTransaction* transaction,
                                       ASurfaceControl* surface_control, float headroom) {
  if (!std::isfinite(headroom) || (headroom < 1.0f && headroom != 0.0f)) {
    Fatal("setDesiredHdrHeadroom: desiredHeadroom isn't finite or >= 1.0f or 0.0f");
  }
  Edit(transaction, surface_control, [](Change*) {});
}

bool ValidFrameRate(float rate, int8_t compatibility, int8_t strategy) {
  // ANativeWindow's ValidateFrameRate: finite, >= 0, a known compatibility
  // (DEFAULT, FIXED_SOURCE, AT_LEAST) and change strategy.
  if (!std::isfinite(rate) || rate < 0) return false;
  if (compatibility < 0 || compatibility > 2) return false;
  if (strategy < 0 || strategy > 1) return false;
  return true;
}

void BIONIC_SYSV SetFrameRateWithChangeStrategy(ASurfaceTransaction* transaction,
                                                ASurfaceControl* surface_control, float rate,
                                                int8_t compatibility, int8_t strategy) {
  if (!ValidFrameRate(rate, compatibility, strategy)) {
    std::fprintf(stderr, "gesher SurfaceControl: setFrameRate: invalid frame rate %f\n", rate);
    return;
  }
  Edit(transaction, surface_control, [&](Change* c) {
    // Composition stays at its 60 Hz pace: the system may choose not to follow.
    c->frame_rate = rate;
    c->fields |= kFrameRate;
  });
}

void BIONIC_SYSV SetFrameRate(ASurfaceTransaction* transaction, ASurfaceControl* surface_control,
                              float rate, int8_t compatibility) {
  SetFrameRateWithChangeStrategy(transaction, surface_control, rate, compatibility, 0);
}

void BIONIC_SYSV ClearFrameRate(ASurfaceTransaction* transaction, ASurfaceControl* surface_control) {
  SetFrameRateWithChangeStrategy(transaction, surface_control, 0, 0, 0);
}

void BIONIC_SYSV SetEnableBackPressure(ASurfaceTransaction* transaction,
                                       ASurfaceControl* surface_control, bool enable) {
  Edit(transaction, surface_control, [&](Change* c) {
    c->back_pressure = enable;  // every applied buffer is presented anyway
    c->fields |= kBackPressure;
  });
}

void BIONIC_SYSV SetFrameTimeline(ASurfaceTransaction* transaction, int64_t vsync_id) {
  Transaction* t = TransactionFromApp(transaction);
  if (t == nullptr) return;
  std::lock_guard<std::mutex> lock(t->mutex);
  t->vsync_id = vsync_id;  // composition is not paced by frame timelines
}

// --- stats ---------------------------------------------------------------------------------------

Stats* StatsFromApp(ASurfaceTransactionStats* p) {
  if (p == nullptr) Fatal("ASurfaceTransactionStats: null stats");
  return reinterpret_cast<Stats*>(p);
}

int64_t BIONIC_SYSV StatsGetLatchTime(ASurfaceTransactionStats* stats) {
  return StatsFromApp(stats)->latch_time;
}

int BIONIC_SYSV StatsGetPresentFenceFd(ASurfaceTransactionStats* stats) {
  if (!StatsFromApp(stats)->completed) {
    Fatal("ASurfaceTransactionStats queried from an incomplete transaction callback");
  }
  return -1;  // no display present fence
}

void BIONIC_SYSV StatsGetSurfaceControls(ASurfaceTransactionStats* stats, ASurfaceControl*** out,
                                         size_t* out_size) {
  Stats* s = StatsFromApp(stats);
  auto** array = new ASurfaceControl*[s->surfaces.size() + 1];
  for (size_t i = 0; i < s->surfaces.size(); ++i) {
    array[i] = reinterpret_cast<ASurfaceControl*>(s->surfaces[i].layer);
  }
  *out = array;
  *out_size = s->surfaces.size();
}

void BIONIC_SYSV StatsReleaseSurfaceControls(ASurfaceControl** surface_controls) {
  delete[] surface_controls;
}

const Stats::Surface& FindSurface(Stats* s, ASurfaceControl* surface_control) {
  for (const Stats::Surface& surface : s->surfaces) {
    if (reinterpret_cast<ASurfaceControl*>(surface.layer) == surface_control) return surface;
  }
  Fatal("ASurfaceControl not found");
  std::abort();
}

int64_t BIONIC_SYSV StatsGetAcquireTime(ASurfaceTransactionStats* stats,
                                        ASurfaceControl* surface_control) {
  return FindSurface(StatsFromApp(stats), surface_control).acquire_time;
}

int BIONIC_SYSV StatsGetPreviousReleaseFenceFd(ASurfaceTransactionStats* stats,
                                               ASurfaceControl* surface_control) {
  Stats* s = StatsFromApp(stats);
  if (!s->completed) Fatal("ASurfaceTransactionStats queried from an incomplete transaction callback");
  FindSurface(s, surface_control);
  return -1;  // released before the callback: the frame without it is composed
}

void Register() {
  struct Entry {
    const char* name;
    void* fn;
  };
#define E(name, fn) {name, reinterpret_cast<void*>(&fn)}
  static const Entry kEntries[] = {
      E("ASurfaceControl_createFromWindow", CreateFromWindow),
      E("ASurfaceControl_create", Create),
      E("ASurfaceControl_acquire", AcquireControl),
      E("ASurfaceControl_release", ReleaseControl),
      E("ASurfaceTransaction_create", TransactionCreate),
      E("ASurfaceTransaction_delete", TransactionDelete),
      E("ASurfaceTransaction_apply", TransactionApply),
      E("ASurfaceTransaction_setOnComplete", SetOnComplete),
      E("ASurfaceTransaction_setOnCommit", SetOnCommit),
      E("ASurfaceTransaction_reparent", Reparent),
      E("ASurfaceTransaction_setVisibility", SetVisibility),
      E("ASurfaceTransaction_setZOrder", SetZOrder),
      E("ASurfaceTransaction_setBuffer", SetBuffer),
      E("ASurfaceTransaction_setBufferWithRelease", SetBufferWithRelease),
      E("ASurfaceTransaction_setColor", SetColor),
      E("ASurfaceTransaction_setGeometry", SetGeometry),
      E("ASurfaceTransaction_setCrop", SetCrop),
      E("ASurfaceTransaction_setPosition", SetPosition),
      E("ASurfaceTransaction_setBufferTransform", SetBufferTransform),
      E("ASurfaceTransaction_setScale", SetScale),
      E("ASurfaceTransaction_setBufferTransparency", SetBufferTransparency),
      E("ASurfaceTransaction_setDamageRegion", SetDamageRegion),
      E("ASurfaceTransaction_setDesiredPresentTime", SetDesiredPresentTime),
      E("ASurfaceTransaction_setBufferAlpha", SetBufferAlpha),
      E("ASurfaceTransaction_setBufferDataSpace", SetBufferDataSpace),
      E("ASurfaceTransaction_setHdrMetadata_smpte2086", SetHdrMetadataSmpte2086),
      E("ASurfaceTransaction_setHdrMetadata_cta861_3", SetHdrMetadataCta8613),
      E("ASurfaceTransaction_setExtendedRangeBrightness", SetExtendedRangeBrightness),
      E("ASurfaceTransaction_setDesiredHdrHeadroom", SetDesiredHdrHeadroom),
      E("ASurfaceTransaction_setFrameRate", SetFrameRate),
      E("ASurfaceTransaction_setFrameRateWithChangeStrategy", SetFrameRateWithChangeStrategy),
      E("ASurfaceTransaction_clearFrameRate", ClearFrameRate),
      E("ASurfaceTransaction_setEnableBackPressure", SetEnableBackPressure),
      E("ASurfaceTransaction_setFrameTimeline", SetFrameTimeline),
      E("ASurfaceTransactionStats_getLatchTime", StatsGetLatchTime),
      E("ASurfaceTransactionStats_getPresentFenceFd", StatsGetPresentFenceFd),
      E("ASurfaceTransactionStats_getASurfaceControls", StatsGetSurfaceControls),
      E("ASurfaceTransactionStats_releaseASurfaceControls", StatsReleaseSurfaceControls),
      E("ASurfaceTransactionStats_getAcquireTime", StatsGetAcquireTime),
      E("ASurfaceTransactionStats_getPreviousReleaseFenceFd", StatsGetPreviousReleaseFenceFd),
  };
#undef E
  for (const Entry& e : kEntries) AddBionicSymbol(e.name, e.fn);
}

[[maybe_unused]] const bool g_registered = (Register(), true);

}  // namespace

ASurfaceControl* ForJavaSurfaceControl(int64_t native_object, const char* name) {
  std::lock_guard<std::mutex> lock(g_tree);
  auto it = JavaLayers().find(native_object);
  if (it != JavaLayers().end()) {
    it->second->refs++;
    return reinterpret_cast<ASurfaceControl*>(it->second);
  }
  static std::atomic<bool> said{false};
  if (!said.exchange(true)) {
    std::fprintf(stderr,
                 "gesher SurfaceControl: ASurfaceControl_fromJava: Gesher does not compose Java "
                 "SurfaceControls; layers under this one are not shown\n");
    std::fflush(stderr);
  }
  Layer* l = NewLayer(name);
  l->java_object = native_object;
  JavaLayers()[native_object] = l;
  return reinterpret_cast<ASurfaceControl*>(l);
}

}  // namespace gesher::bionic::sc
