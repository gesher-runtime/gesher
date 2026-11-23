// gesher: AHardwareBuffer (the NDK's <android/hardware_buffer.h>, in
// libnativewindow.so and libandroid.so) for an app's ELF libraries (ADR 0089).
//
// A buffer is shared memory: a memfd of stride x height pixels (stride ==
// width) mapped in this process, described by its AHardwareBuffer_Desc and a
// system-wide id (pid << 32 | count, as GraphicBuffer's). What can be
// allocated is what this implementation can serve, and AHardwareBuffer_isSupported
// answers the same:
//  - formats R8G8B8A8, R8G8B8X8, R8G8B8, R5G6B5, R16G16B16A16_FLOAT,
//    R10G10B10A2, R8 and BLOB, one layer (no arrays, cube maps or mipmaps);
//  - CPU read/write usage; GPU sampling, GPU color output and composer overlay
//    (SurfaceControl) for every format but BLOB, when ANGLE's device is there
//    (the GPU thread, gpu_interop.h) and within its texture size; float color
//    output only with float color buffers;
//  - not protected content, video encoding, sensor data, GPU data buffers or
//    front-buffer rendering: those allocations fail.
// The buffer's GPU form is a texture of the GPU thread's Direct3D 11 device,
// shared with ANGLE's device, where each eglCreateImageKHR of the buffer makes
// an EGLImage of it (hardware_buffer.h explains when memory and texture are
// synchronized). RGBX, RGB and RGB565 buffers are RGBA8 textures whose images
// are RGB (alpha 1), converted at uploads and read-backs.
// AHardwareBuffer_sendHandleToUnixSocket sends the description and the memfd
// over the socket (SCM_RIGHTS, ADR 0088's broker between processes);
// AHardwareBuffer_recvHandleFromUnixSocket maps the same memory.
#include "gpu_interop.h"  // first: the EGL/GL prototype macros
#include "hardware_buffer.h"

#include "abi_types.h"

#include <gesher/bionic_shim.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef EGL_D3D11_TEXTURE_ANGLE
#define EGL_D3D11_TEXTURE_ANGLE 0x3484
#endif
#ifndef EGL_TEXTURE_INTERNAL_FORMAT_ANGLE
#define EGL_TEXTURE_INTERNAL_FORMAT_ANGLE 0x345D
#endif

namespace gesher::bionic::ahb {

namespace {

constexpr uint32_t kMagic = 0x42484147;  // "GAHB"

// Android's status_t values as the NDK returns them.
constexpr int kBadValue = -EINVAL;
constexpr int kInvalidOperation = -ENOSYS;

constexpr uint64_t kCpuMask = AHARDWAREBUFFER_USAGE_CPU_READ_MASK | AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK;
constexpr uint64_t kGpuUsage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                               AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                               AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
constexpr uint64_t kVendorUsage = (0xFULL << 28) | (0xFFFFULL << 48);  // VENDOR_0..19
// What this implementation serves.
constexpr uint64_t kServedUsage = kCpuMask | kGpuUsage | kVendorUsage;

struct Format {
  uint32_t format;
  int bytes;                  // per pixel (BLOB: per byte)
  bool gpu;                   // has a texture form
  gpu::TextureFormat texture;  // how the texture keeps it
  int texture_bytes;          // per texel
  uint32_t image_format;      // the GL internal format apps' images see, if not the texture's
  bool opaque;                // no alpha channel
};

const Format kFormats[] = {
    {AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 4, true, gpu::TextureFormat::kRgba8, 4, 0, false},
    // RGBA8 textures whose images are RGB (sampled alpha 1).
    {AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 4, true, gpu::TextureFormat::kRgba8, 4, GL_RGB, true},
    {AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM, 3, true, gpu::TextureFormat::kRgba8, 4, GL_RGB, true},
    {AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM, 2, true, gpu::TextureFormat::kRgba8, 4, GL_RGB, true},
    {AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT, 8, true, gpu::TextureFormat::kRgba16f, 8, 0, false},
    {AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM, 4, true, gpu::TextureFormat::kRgb10a2, 4, 0, false},
    {AHARDWAREBUFFER_FORMAT_R8_UNORM, 1, true, gpu::TextureFormat::kR8, 1, 0, true},
    {AHARDWAREBUFFER_FORMAT_BLOB, 1, false, gpu::TextureFormat::kRgba8, 0, 0, true},
};

const Format* FindFormat(uint32_t format) {
  for (const Format& f : kFormats) {
    if (f.format == format) return &f;
  }
  return nullptr;
}

// Every format Android defines (AHardwareBuffer_isValidPixelFormat), served or not.
bool KnownFormat(uint32_t format) {
  switch (format) {
    case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
    case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
    case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
    case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
    case AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT:
    case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM:
    case AHARDWAREBUFFER_FORMAT_BLOB:
    case AHARDWAREBUFFER_FORMAT_D16_UNORM:
    case AHARDWAREBUFFER_FORMAT_D24_UNORM:
    case AHARDWAREBUFFER_FORMAT_D24_UNORM_S8_UINT:
    case AHARDWAREBUFFER_FORMAT_D32_FLOAT:
    case AHARDWAREBUFFER_FORMAT_D32_FLOAT_S8_UINT:
    case AHARDWAREBUFFER_FORMAT_S8_UINT:
    case AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420:
    case AHARDWAREBUFFER_FORMAT_YCbCr_P010:
    case AHARDWAREBUFFER_FORMAT_YCbCr_P210:
    case AHARDWAREBUFFER_FORMAT_R8_UNORM:
    case AHARDWAREBUFFER_FORMAT_R16_UINT:
    case AHARDWAREBUFFER_FORMAT_R16G16_UINT:
    case AHARDWAREBUFFER_FORMAT_R10G10B10A10_UNORM:
    case AHARDWAREBUFFER_FORMAT_B10G10R10A2_UNORM:
    case AHARDWAREBUFFER_FORMAT_B10G10R10X2_UNORM:
    case 0x14:          // YCbCr_422_I
    case 0x32315659:    // YV12
      return true;
    default:
      return false;
  }
}

bool YuvFormat(uint32_t format) {
  return format == AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420 || format == AHARDWAREBUFFER_FORMAT_YCbCr_P010 ||
         format == AHARDWAREBUFFER_FORMAT_YCbCr_P210 || format == 0x14 || format == 0x32315659;
}

void LogOnce(std::atomic<bool>* said, const char* text) {
  if (!said->exchange(true)) {
    std::fprintf(stderr, "gesher AHardwareBuffer: %s\n", text);
    std::fflush(stderr);
  }
}

// AOSP's AHardwareBuffer_isValidDescription.
bool ValidDescription(const AHardwareBuffer_Desc* desc) {
  if (desc->width == 0 || desc->height == 0 || desc->layers == 0) return false;
  if (desc->rfu0 != 0 || desc->rfu1 != 0) return false;
  if (!KnownFormat(desc->format)) return false;
  if (desc->format == AHARDWAREBUFFER_FORMAT_BLOB) {
    if (desc->height != 1 || desc->layers != 1) return false;
    const uint64_t gpu = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                         AHARDWAREBUFFER_USAGE_GPU_CUBE_MAP | AHARDWAREBUFFER_USAGE_GPU_MIPMAP_COMPLETE;
    if ((desc->usage & gpu) != 0) return false;
    if ((desc->usage & AHARDWAREBUFFER_USAGE_VIDEO_ENCODE) != 0) return false;
  } else {
    if (YuvFormat(desc->format)) {
      if (desc->layers != 1) return false;
      const uint64_t bad = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_CUBE_MAP |
                           AHARDWAREBUFFER_USAGE_GPU_MIPMAP_COMPLETE;
      if ((desc->usage & bad) != 0) return false;
    }
    if ((desc->usage & AHARDWAREBUFFER_USAGE_SENSOR_DIRECT_DATA) != 0) return false;
  }
  if ((desc->usage & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT) != 0 && (desc->usage & kCpuMask) != 0) {
    return false;
  }
  if ((desc->usage & AHARDWAREBUFFER_USAGE_GPU_CUBE_MAP) != 0) {
    if (desc->width != desc->height || desc->layers % 6 != 0) return false;
  }
  return true;
}

// Whether this implementation can allocate a valid description.
bool Served(const AHardwareBuffer_Desc* desc) {
  const Format* f = FindFormat(desc->format);
  if (f == nullptr || desc->layers != 1) return false;
  if ((desc->usage & ~kServedUsage) != 0) return false;
  const uint64_t bytes = static_cast<uint64_t>(desc->width) * desc->height * f->bytes;
  if (bytes > (1ULL << 30)) return false;
  if ((desc->usage & kGpuUsage) != 0) {
    if (!f->gpu) return false;
    gpu::Thread* thread = gpu::Thread::Get();
    if (thread == nullptr) return false;
    if (desc->width > static_cast<uint32_t>(thread->max_texture_size()) ||
        desc->height > static_cast<uint32_t>(thread->max_texture_size())) {
      return false;
    }
    if (f->texture == gpu::TextureFormat::kRgba16f && (desc->usage & AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER) != 0 &&
        !thread->color_buffer_float()) {
      return false;
    }
  }
  return true;
}

uint64_t NextId() {
  static std::atomic<uint32_t> count{0};
  return (static_cast<uint64_t>(getpid()) << 32) | (count.fetch_add(1) + 1);
}

}  // namespace

struct GpuState {
  gpu::Texture* texture = nullptr;
};

// What an AHardwareBuffer* points to on Android is a GraphicBuffer: its
// RefBase (a vtable and mRefs) and then its ANativeWindowBuffer, 16 bytes in,
// which eglGetNativeClientBufferANDROID gives as the EGLClientBuffer. Code
// in apps relies on that layout: ANGLE (bundled with Chromium, running on the
// system's GLES) computes the client buffer as the buffer + 16 and reads the
// ANativeWindowBuffer's magic, size, format, layer count and usage
// (angle/src/common/android_util.cpp). A buffer here starts with the same
// layout, the ANativeWindowBuffer filled in as GraphicBuffer fills it.
struct NativeHandle {  // native_handle_t with one fd
  int version = static_cast<int>(3 * sizeof(int));
  int num_fds = 1;
  int num_ints = 0;
  int fd = -1;
};

// <nativebase/nativebase.h>'s ANativeWindowBuffer (LP64).
struct NativeWindowBuffer {
  int magic;
  int version;
  void* reserved[4];
  void (*inc_ref)(void* base);  // the app's (System V) code calls these
  void (*dec_ref)(void* base);
  int width;
  int height;
  int stride;
  int format;
  int usage_deprecated;
  uintptr_t layer_count;
  void* reserved1[1];
  const NativeHandle* handle;
  uint64_t usage;
  void* reserved_proc[7];
};
static_assert(sizeof(NativeWindowBuffer) == 168, "ANativeWindowBuffer's LP64 size");
static_assert(offsetof(NativeWindowBuffer, width) == 56 && offsetof(NativeWindowBuffer, layer_count) == 80 &&
                  offsetof(NativeWindowBuffer, handle) == 96 && offsetof(NativeWindowBuffer, usage) == 104,
              "ANativeWindowBuffer's LP64 layout");

struct GraphicBufferHead {
  const void* vtable = nullptr;  // GraphicBuffer's; never called
  void* refs = nullptr;          // RefBase::mRefs
  NativeWindowBuffer native;
};
static_assert(offsetof(GraphicBufferHead, native) == 16, "ANativeWindowBuffer 16 bytes into a GraphicBuffer");

constexpr int kNativeBufferMagic = ('_' << 24) | ('b' << 16) | ('f' << 8) | 'r';

struct Buffer {
  GraphicBufferHead head;  // first: an AHardwareBuffer* is the buffer's address
  NativeHandle handle;
  uint32_t magic = kMagic;
  std::atomic<int> refs{1};
  AHardwareBuffer_Desc desc{};
  const Format* format = nullptr;
  uint64_t id = 0;
  int fd = -1;
  uint8_t* data = nullptr;
  size_t size = 0;
  bool foreign = false;  // received from another process

  std::mutex mutex;  // the lock state and flags below
  int read_locks = 0, write_locks = 0;
  ARect write_rect{0, 0, 0, 0};
  bool memory_newer = false;  // memory has writes the texture lacks
  bool gpu_written = false;   // an app image exists or existed: the texture may be newer
  bool has_texture = false;   // gpu.texture exists (also readable off the GPU thread)
  int app_images = 0;         // the app's EGLImages of it
  GpuState gpu;               // GPU thread only

  size_t row_bytes() const { return static_cast<size_t>(desc.stride) * format->bytes; }
};

namespace {

std::mutex g_registry_mutex;
std::unordered_set<Buffer*>& Registry() {
  static auto* set = new std::unordered_set<Buffer*>();
  return *set;
}

struct ImageEntry {
  Buffer* buffer = nullptr;
  EGLDisplay display = EGL_NO_DISPLAY;
};
std::mutex g_images_mutex;
std::unordered_map<EGLImageKHR, ImageEntry>& Images() {
  static auto* map = new std::unordered_map<EGLImageKHR, ImageEntry>();
  return *map;
}

ARect Clip(const Buffer* b, const ARect* rect) {
  ARect r{0, 0, static_cast<int32_t>(b->desc.width), static_cast<int32_t>(b->desc.height)};
  if (rect == nullptr) return r;
  r.left = std::max<int32_t>(0, rect->left);
  r.top = std::max<int32_t>(0, rect->top);
  r.right = std::min<int32_t>(static_cast<int32_t>(b->desc.width), rect->right);
  r.bottom = std::min<int32_t>(static_cast<int32_t>(b->desc.height), rect->bottom);
  if (r.right < r.left) r.right = r.left;
  if (r.bottom < r.top) r.bottom = r.top;
  return r;
}

bool Empty(const ARect& r) { return r.right <= r.left || r.bottom <= r.top; }

ARect Union(const ARect& a, const ARect& b) {
  if (Empty(a)) return b;
  if (Empty(b)) return a;
  return ARect{std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right),
               std::max(a.bottom, b.bottom)};
}

// GESHER_TRACE_SURFACE_CONTROL: buffers' textures and images, on stderr.
bool Trace() {
  static const bool on = std::getenv("GESHER_TRACE_SURFACE_CONTROL") != nullptr;
  return on;
}

// --- on the GPU thread ----------------------------------------------------------------------------

// The texture's layout of rect `r` of the buffer's memory (converted when the
// texture stores the format another way); `row_bytes` set.
std::vector<uint8_t> ToTexture(const Buffer* b, const ARect& r, size_t* row_bytes) {
  const Format* f = b->format;
  const int w = r.right - r.left, h = r.bottom - r.top;
  const int tb = f->texture_bytes;
  std::vector<uint8_t> out(static_cast<size_t>(w) * h * tb);
  *row_bytes = static_cast<size_t>(w) * tb;
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = b->data + (static_cast<size_t>(r.top + y) * b->desc.stride + r.left) * f->bytes;
    uint8_t* d = &out[static_cast<size_t>(y) * w * tb];
    switch (f->format) {
      case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
        // X is undefined in memory; the texture's alpha is 1, which ANGLE
        // keeps for an RGB image (it masks alpha writes to emulated RGB).
        for (int x = 0; x < w; ++x) {
          std::memcpy(d + x * 4, s + x * 4, 3);
          d[x * 4 + 3] = 0xff;
        }
        break;
      case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
        for (int x = 0; x < w; ++x) {
          d[x * 4 + 0] = s[x * 3 + 0];
          d[x * 4 + 1] = s[x * 3 + 1];
          d[x * 4 + 2] = s[x * 3 + 2];
          d[x * 4 + 3] = 0xff;
        }
        break;
      case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
        for (int x = 0; x < w; ++x) {
          uint16_t v;
          std::memcpy(&v, s + x * 2, 2);
          const int r5 = v >> 11, g6 = (v >> 5) & 63, b5 = v & 31;
          d[x * 4 + 0] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
          d[x * 4 + 1] = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
          d[x * 4 + 2] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
          d[x * 4 + 3] = 0xff;
        }
        break;
      default:  // the same layout
        std::memcpy(d, s, static_cast<size_t>(w) * tb);
        break;
    }
  }
  return out;
}

// Rect `r` read from the texture (its layout) into the buffer's memory.
void FromTexture(Buffer* b, const ARect& r, const uint8_t* px) {
  const Format* f = b->format;
  const int w = r.right - r.left, h = r.bottom - r.top;
  const int tb = f->texture_bytes;
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = px + static_cast<size_t>(y) * w * tb;
    uint8_t* d = b->data + (static_cast<size_t>(r.top + y) * b->desc.stride + r.left) * f->bytes;
    switch (f->format) {
      case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
        for (int x = 0; x < w; ++x) {
          d[x * 3 + 0] = s[x * 4 + 0];
          d[x * 3 + 1] = s[x * 4 + 1];
          d[x * 3 + 2] = s[x * 4 + 2];
        }
        break;
      case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
        for (int x = 0; x < w; ++x) {
          const uint8_t* p = s + x * 4;
          const uint16_t v = static_cast<uint16_t>(((p[0] * 31 + 127) / 255) << 11 |
                                                   ((p[1] * 63 + 127) / 255) << 5 | ((p[2] * 31 + 127) / 255));
          std::memcpy(d + x * 2, &v, 2);
        }
        break;
      default:
        std::memcpy(d, s, static_cast<size_t>(w) * tb);
        break;
    }
  }
}

// Uploads rect `r` of the buffer's memory into its texture.
void UploadRect(Buffer* b, const ARect& r) {
  if (b->gpu.texture == nullptr || Empty(r)) return;
  size_t row_bytes = 0;
  std::vector<uint8_t> px = ToTexture(b, r, &row_bytes);
  gpu::UpdateTexture(b->gpu.texture, r.left, r.top, r.right - r.left, r.bottom - r.top, px.data(), row_bytes);
}

// Creates the texture (with the memory's contents) if the buffer has none.
bool EnsureTexture(Buffer* b) {
  if (b->gpu.texture != nullptr) return true;
  if (!b->format->gpu) return false;
  size_t row_bytes = 0;
  std::vector<uint8_t> px = ToTexture(b, Clip(b, nullptr), &row_bytes);
  gpu::Texture* texture = gpu::CreateTexture(b->format->texture, static_cast<int>(b->desc.width),
                                             static_cast<int>(b->desc.height), px.data(), row_bytes);
  if (texture == nullptr) {
    std::fprintf(stderr, "gesher AHardwareBuffer: no %ux%u texture (format 0x%x)\n", b->desc.width,
                 b->desc.height, b->desc.format);
    std::fflush(stderr);
    return false;
  }
  b->gpu.texture = texture;
  {
    std::lock_guard<std::mutex> lock(b->mutex);
    b->memory_newer = false;
    b->has_texture = true;
  }
  if (Trace()) {
    std::fprintf(stderr, "gesher AHardwareBuffer: buffer 0x%llx %ux%u format 0x%x: texture %p\n",
                 static_cast<unsigned long long>(b->id), b->desc.width, b->desc.height, b->desc.format,
                 static_cast<void*>(texture));
  }
  return true;
}

// Reads rect `r` of the texture back into memory.
void ReadbackRect(Buffer* b, const ARect& r) {
  if (b->gpu.texture == nullptr || Empty(r)) return;
  const int w = r.right - r.left, h = r.bottom - r.top;
  const size_t row_bytes = static_cast<size_t>(w) * b->format->texture_bytes;
  std::vector<uint8_t> px(row_bytes * h);
  if (!gpu::ReadTexture(b->gpu.texture, r.left, r.top, w, h, px.data(), row_bytes)) {
    static std::atomic<bool> said{false};
    LogOnce(&said, "a buffer's texture could not be read back: CPU reads see the last CPU contents");
    return;
  }
  FromTexture(b, r, px.data());
}

void FreeGpu(Buffer* b) {
  gpu::DestroyTexture(b->gpu.texture);
  b->gpu = GpuState();
}

void Destroy(Buffer* b) {
  {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    Registry().erase(b);
  }
  {
    // A stale eglDestroyImageKHR no longer finds the buffer's image.
    std::lock_guard<std::mutex> lock(g_images_mutex);
    for (auto it = Images().begin(); it != Images().end();) {
      it = it->second.buffer == b ? Images().erase(it) : std::next(it);
    }
  }
  b->magic = 0;
  auto free_memory = [b] {
    if (b->data != nullptr) munmap(b->data, b->size);
    if (b->fd >= 0) close(b->fd);
    delete b;
  };
  bool has_texture;
  {
    std::lock_guard<std::mutex> lock(b->mutex);
    has_texture = b->has_texture;
  }
  if (!has_texture) {
    free_memory();
    return;
  }
  gpu::Thread::Get()->Post([b, free_memory] {
    FreeGpu(b);
    free_memory();
  });
}

Buffer* BufferOfNative(void* base) {
  return reinterpret_cast<Buffer*>(static_cast<char*>(base) - offsetof(GraphicBufferHead, native));
}
void BIONIC_SYSV NativeIncRef(void* base) { BufferOfNative(base)->refs.fetch_add(1, std::memory_order_relaxed); }
void BIONIC_SYSV NativeDecRef(void* base);

Buffer* Create(const AHardwareBuffer_Desc& desc, int fd, size_t size, uint64_t id, bool foreign,
               int* error) {
  void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    *error = -errno;
    return nullptr;
  }
  auto* b = new Buffer();
  b->desc = desc;
  b->format = FindFormat(desc.format);
  NativeWindowBuffer& n = b->head.native;
  std::memset(&n, 0, sizeof n);
  n.magic = kNativeBufferMagic;
  n.version = static_cast<int>(sizeof(NativeWindowBuffer));
  n.inc_ref = reinterpret_cast<void (*)(void*)>(&NativeIncRef);
  n.dec_ref = reinterpret_cast<void (*)(void*)>(&NativeDecRef);
  n.width = static_cast<int>(desc.width);
  n.height = static_cast<int>(desc.height);
  n.stride = static_cast<int>(desc.stride);
  n.format = static_cast<int>(desc.format);  // the HAL pixel format's value
  n.usage_deprecated = static_cast<int>(desc.usage & 0xffffffffu);
  n.layer_count = desc.layers;
  n.usage = desc.usage;  // gralloc's bits have the same values
  b->handle.fd = fd;
  n.handle = &b->handle;
  b->id = id;
  b->fd = fd;
  b->data = static_cast<uint8_t*>(map);
  b->size = size;
  b->foreign = foreign;
  b->memory_newer = foreign;
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  Registry().insert(b);
  return b;
}

// The bytes a description takes.
size_t BufferBytes(const AHardwareBuffer_Desc& desc, const Format* f) {
  size_t bytes = static_cast<size_t>(desc.stride) * desc.height * f->bytes;
  return bytes == 0 ? 1 : bytes;
}

// Waits for a fence fd (any pollable fd; ADR 0089: Gesher's fences signal once
// their work is done) and closes it, as Android's gralloc lock owns it.
void WaitFenceAndClose(int fence) {
  if (fence < 0) return;
  pollfd p{fence, POLLIN, 0};
  while (poll(&p, 1, -1) < 0 && errno == EINTR) {
  }
  close(fence);
}

}  // namespace

Buffer* FromApp(const void* p) {
  if (p == nullptr) return nullptr;
  auto* b = const_cast<Buffer*>(static_cast<const Buffer*>(p));
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  return Registry().count(b) != 0 && b->magic == kMagic ? b : nullptr;
}

void Acquire(Buffer* b) { b->refs.fetch_add(1, std::memory_order_relaxed); }

Buffer* FromClientBuffer(const void* client_buffer) {
  if (client_buffer == nullptr) return nullptr;
  return FromApp(static_cast<const char*>(client_buffer) - offsetof(GraphicBufferHead, native));
}

void* ClientBuffer(Buffer* b) { return &b->head.native; }

namespace {
void BIONIC_SYSV NativeDecRef(void* base) { Release(BufferOfNative(base)); }
}  // namespace

void Release(Buffer* b) {
  if (b->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) Destroy(b);
}

const AHardwareBuffer_Desc& Desc(const Buffer* b) { return b->desc; }

EGLImageKHR AcquireAppImage(Buffer* b, EGLDisplay display, EGLint* egl_error) {
  if (!b->format->gpu || (b->desc.usage & kGpuUsage) == 0) {
    *egl_error = EGL_BAD_PARAMETER;  // allocated without GPU usage
    return EGL_NO_IMAGE_KHR;
  }
  gpu::Thread* thread = gpu::Thread::Get();
  if (thread == nullptr) {
    *egl_error = EGL_BAD_ALLOC;
    return EGL_NO_IMAGE_KHR;
  }
  if (display != thread->display()) {
    *egl_error = EGL_BAD_DISPLAY;
    return EGL_NO_IMAGE_KHR;
  }
  void* angle_texture = nullptr;
  thread->Run([&] {
    if (EnsureTexture(b)) angle_texture = gpu::AngleTexture(b->gpu.texture);
  });
  if (angle_texture == nullptr) {
    *egl_error = EGL_BAD_ALLOC;
    return EGL_NO_IMAGE_KHR;
  }
  // A new image of the texture ANGLE's device shares (no context of the
  // app's or of the GPU thread is involved).
  std::vector<EGLint> attribs;
  if (b->format->image_format != 0) {
    attribs.push_back(EGL_TEXTURE_INTERNAL_FORMAT_ANGLE);
    attribs.push_back(static_cast<EGLint>(b->format->image_format));
  }
  attribs.push_back(EGL_NONE);
  const gpu::Gl* g = gpu::Functions();
  EGLImageKHR image = g->eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_D3D11_TEXTURE_ANGLE,
                                           static_cast<EGLClientBuffer>(angle_texture), attribs.data());
  if (image == EGL_NO_IMAGE_KHR) {
    *egl_error = g->eglGetError();
    std::fprintf(stderr, "gesher AHardwareBuffer: no EGLImage of a %ux%u format 0x%x texture: 0x%x\n",
                 b->desc.width, b->desc.height, b->desc.format, *egl_error);
    std::fflush(stderr);
    return EGL_NO_IMAGE_KHR;
  }
  Acquire(b);  // the app's image holds the buffer, as on Android
  int images;
  {
    std::lock_guard<std::mutex> lock(b->mutex);
    b->gpu_written = true;  // the app may render into it from now on
    images = ++b->app_images;
  }
  if (Trace()) {
    std::fprintf(stderr, "gesher AHardwareBuffer: buffer 0x%llx: EGLImage %p, %d app image(s)\n",
                 static_cast<unsigned long long>(b->id), image, images);
  }
  std::lock_guard<std::mutex> lock(g_images_mutex);
  Images()[image] = ImageEntry{b, display};
  return image;
}

int ReleaseAppImage(EGLImageKHR image) {
  ImageEntry entry;
  {
    std::lock_guard<std::mutex> lock(g_images_mutex);
    auto it = Images().find(image);
    if (it == Images().end()) return 0;
    entry = it->second;
    Images().erase(it);
  }
  gpu::Functions()->eglDestroyImageKHR(entry.display, image);
  {
    std::lock_guard<std::mutex> lock(entry.buffer->mutex);
    entry.buffer->app_images--;
  }
  Release(entry.buffer);  // the image's reference
  return 1;
}

gpu::Texture* CompositorTexture(Buffer* b, bool* opaque_format) {
  *opaque_format = b->format->opaque;
  if (!b->format->gpu) return nullptr;
  if (!EnsureTexture(b)) return nullptr;
  bool upload;
  {
    std::lock_guard<std::mutex> lock(b->mutex);
    upload = b->memory_newer;
    b->memory_newer = false;
  }
  if (upload) UploadRect(b, Clip(b, nullptr));
  return b->gpu.texture;
}

void NoteSubmitted(Buffer* b) {
  // Another process wrote the memory (with its CPU or after reading its GPU
  // texture back); this process's texture, if any, is older.
  std::lock_guard<std::mutex> lock(b->mutex);
  if (b->foreign && !b->gpu_written) b->memory_newer = true;
}

// --- the NDK functions ----------------------------------------------------------------------------

namespace {

int BIONIC_SYSV Allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** out) {
  if (desc == nullptr || out == nullptr) return kBadValue;
  *out = nullptr;
  if (!ValidDescription(desc)) return kBadValue;
  if (!Served(desc)) {
    std::fprintf(stderr,
                 "gesher AHardwareBuffer: %ux%u format 0x%x usage 0x%llx layers %u is not supported\n",
                 desc->width, desc->height, desc->format,
                 static_cast<unsigned long long>(desc->usage), desc->layers);
    std::fflush(stderr);
    return kBadValue;
  }
  AHardwareBuffer_Desc d = *desc;
  d.stride = d.width;
  const Format* f = FindFormat(d.format);
  const size_t size = BufferBytes(d, f);
  int fd = memfd_create("AHardwareBuffer", MFD_CLOEXEC);
  if (fd < 0) return -errno;
  if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
    int e = errno;
    close(fd);
    return -e;
  }
  int error = 0;
  Buffer* b = Create(d, fd, size, NextId(), /*foreign=*/false, &error);
  if (b == nullptr) {
    close(fd);
    return error != 0 ? error : -ENOMEM;
  }
  *out = ToApp(b);
  return 0;
}

int BIONIC_SYSV IsSupported(const AHardwareBuffer_Desc* desc) {
  if (desc == nullptr || !ValidDescription(desc)) return 0;
  return Served(desc) ? 1 : 0;
}

void BIONIC_SYSV AcquireSysv(AHardwareBuffer* buffer) {
  if (Buffer* b = FromApp(buffer)) Acquire(b);
}

void BIONIC_SYSV ReleaseSysv(AHardwareBuffer* buffer) {
  if (Buffer* b = FromApp(buffer)) Release(b);
}

void BIONIC_SYSV Describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* out) {
  Buffer* b = FromApp(buffer);
  if (b == nullptr || out == nullptr) return;
  *out = b->desc;
  out->rfu0 = 0;
  out->rfu1 = 0;
}

// The common half of the lock functions.
int LockImpl(Buffer* b, uint64_t usage, int32_t fence, const ARect* rect, void** out) {
  if (out != nullptr) *out = nullptr;
  if ((usage & ~kCpuMask) != 0 || usage == 0) {
    WaitFenceAndClose(fence);
    return kBadValue;
  }
  if (b->desc.layers > 1) {
    WaitFenceAndClose(fence);
    return kInvalidOperation;
  }
  // The flags must match the buffer's usage (a read lock needs CPU read usage).
  if (((usage & AHARDWAREBUFFER_USAGE_CPU_READ_MASK) != 0 &&
       (b->desc.usage & AHARDWAREBUFFER_USAGE_CPU_READ_MASK) == 0) ||
      ((usage & AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK) != 0 &&
       (b->desc.usage & AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK) == 0)) {
    WaitFenceAndClose(fence);
    return kBadValue;
  }
  WaitFenceAndClose(fence);
  const ARect r = Clip(b, rect);
  bool readback;
  {
    std::lock_guard<std::mutex> lock(b->mutex);
    readback = b->has_texture && b->gpu_written;
  }
  // What the GPU drew is in the texture; the CPU sees it once read back.
  if (readback) gpu::Thread::Get()->Run([&] { ReadbackRect(b, r); });
  {
    std::lock_guard<std::mutex> lock(b->mutex);
    if ((usage & AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK) != 0) {
      b->write_locks++;
      b->write_rect = Union(b->write_rect, r);
    } else {
      b->read_locks++;
    }
  }
  if (out != nullptr) *out = b->data;
  return 0;
}

int BIONIC_SYSV Lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence, const ARect* rect,
                     void** out) {
  Buffer* b = FromApp(buffer);
  if (b == nullptr || out == nullptr) {
    WaitFenceAndClose(fence);
    return kBadValue;
  }
  return LockImpl(b, usage, fence, rect, out);
}

int BIONIC_SYSV LockAndGetInfo(AHardwareBuffer* buffer, uint64_t usage, int32_t fence,
                               const ARect* rect, void** out, int32_t* bytes_per_pixel,
                               int32_t* bytes_per_stride) {
  if (bytes_per_pixel != nullptr) *bytes_per_pixel = -1;
  if (bytes_per_stride != nullptr) *bytes_per_stride = -1;
  Buffer* b = FromApp(buffer);
  if (b == nullptr) {
    WaitFenceAndClose(fence);
    return kBadValue;
  }
  int result = LockImpl(b, usage, fence, rect, out);
  if (result == 0) {
    if (bytes_per_pixel != nullptr) *bytes_per_pixel = b->format->bytes;
    if (bytes_per_stride != nullptr) *bytes_per_stride = static_cast<int32_t>(b->row_bytes());
  }
  return result;
}

int BIONIC_SYSV LockPlanes(AHardwareBuffer* buffer, uint64_t usage, int32_t fence, const ARect* rect,
                           AHardwareBuffer_Planes* planes) {
  Buffer* b = FromApp(buffer);
  if (b == nullptr || planes == nullptr) {
    WaitFenceAndClose(fence);
    return kBadValue;
  }
  std::memset(planes->planes, 0, sizeof planes->planes);
  planes->planeCount = 0;
  void* address = nullptr;
  int result = LockImpl(b, usage, fence, rect, &address);
  if (result != 0) return result;
  // Every served format has one plane.
  planes->planeCount = 1;
  planes->planes[0].data = address;
  planes->planes[0].pixelStride = static_cast<uint32_t>(b->format->bytes);
  planes->planes[0].rowStride = static_cast<uint32_t>(b->row_bytes());
  return 0;
}

int BIONIC_SYSV Unlock(AHardwareBuffer* buffer, int32_t* fence) {
  Buffer* b = FromApp(buffer);
  if (fence != nullptr) *fence = -1;
  if (b == nullptr) return kBadValue;
  ARect upload{0, 0, 0, 0};
  bool has_texture;
  {
    std::lock_guard<std::mutex> lock(b->mutex);
    if (b->write_locks > 0) {
      if (--b->write_locks == 0) {
        upload = b->write_rect;
        b->write_rect = ARect{0, 0, 0, 0};
      }
    } else if (b->read_locks > 0) {
      b->read_locks--;
    } else {
      return kBadValue;  // not locked
    }
    has_texture = b->has_texture;
    if (!Empty(upload) && !has_texture) b->memory_newer = true;
  }
  // The CPU's writes reach the texture before unlock returns (fence -1: done).
  if (!Empty(upload) && has_texture) gpu::Thread::Get()->Run([&] { UploadRect(b, upload); });
  return 0;
}

int BIONIC_SYSV GetId(const AHardwareBuffer* buffer, uint64_t* out_id) {
  Buffer* b = FromApp(buffer);
  if (b == nullptr || out_id == nullptr) return kBadValue;
  *out_id = b->id;
  return 0;
}

// What travels with the memfd.
struct Wire {
  uint32_t magic;
  uint32_t version;
  uint32_t width, height, layers, format;
  uint64_t usage;
  uint32_t stride, reserved;
  uint64_t id;
  uint64_t size;
};
constexpr uint32_t kWireMagic = 0x57424841;  // "AHBW"

int BIONIC_SYSV SendHandle(const AHardwareBuffer* buffer, int socket_fd) {
  Buffer* b = FromApp(buffer);
  if (b == nullptr) return kBadValue;
  bool readback;
  {
    std::lock_guard<std::mutex> lock(b->mutex);
    readback = b->has_texture && b->gpu_written;
  }
  // The receiver maps the memory: it gets what the GPU drew so far.
  if (readback) gpu::Thread::Get()->Run([&] { ReadbackRect(b, Clip(b, nullptr)); });
  Wire wire{kWireMagic, 1, b->desc.width, b->desc.height, b->desc.layers, b->desc.format,
            b->desc.usage, b->desc.stride, 0, b->id, b->size};
  iovec iov{&wire, sizeof wire};
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof control;
  cmsghdr* c = CMSG_FIRSTHDR(&msg);
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof(int));
  std::memcpy(CMSG_DATA(c), &b->fd, sizeof(int));
  ssize_t n;
  do {
    n = sendmsg(socket_fd, &msg, MSG_NOSIGNAL);
  } while (n < 0 && errno == EINTR);
  if (n < 0) {
    int e = errno;
    std::fprintf(stderr, "gesher AHardwareBuffer: sending a buffer on fd %d failed: errno %d\n",
                 socket_fd, e);
    std::fflush(stderr);
    return -e;
  }
  if (static_cast<size_t>(n) != sizeof wire) return kInvalidOperation;
  return 0;
}

int BIONIC_SYSV RecvHandle(int socket_fd, AHardwareBuffer** out) {
  if (out == nullptr) return kBadValue;
  *out = nullptr;
  Wire wire{};
  iovec iov{&wire, sizeof wire};
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 4)] = {};
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof control;
  ssize_t n;
  do {
    n = recvmsg(socket_fd, &msg, MSG_CMSG_CLOEXEC);
  } while (n < 0 && errno == EINTR);
  if (n < 0) return -errno;
  int fd = -1;
  for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
    const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    for (size_t i = 0; i < count; ++i) {
      int received;
      std::memcpy(&received, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
      if (fd < 0) {
        fd = received;
      } else {
        close(received);  // only one belongs to a buffer
      }
    }
  }
  // A stream socket may deliver the rest of the description later.
  size_t got = static_cast<size_t>(n);
  while (got > 0 && got < sizeof wire) {
    ssize_t more = recv(socket_fd, reinterpret_cast<char*>(&wire) + got, sizeof wire - got, 0);
    if (more < 0 && errno == EINTR) continue;
    if (more <= 0) break;
    got += static_cast<size_t>(more);
  }
  if (got != sizeof wire || wire.magic != kWireMagic || wire.version != 1 || fd < 0) {
    if (fd >= 0) close(fd);
    std::fprintf(stderr, "gesher AHardwareBuffer: no buffer in what fd %d received\n", socket_fd);
    std::fflush(stderr);
    return kInvalidOperation;
  }
  AHardwareBuffer_Desc desc{};
  desc.width = wire.width;
  desc.height = wire.height;
  desc.layers = wire.layers;
  desc.format = wire.format;
  desc.usage = wire.usage;
  desc.stride = wire.stride;
  const Format* f = FindFormat(desc.format);
  if (f == nullptr || desc.stride < desc.width || wire.size < BufferBytes(desc, f)) {
    close(fd);
    return kInvalidOperation;
  }
  int error = 0;
  Buffer* b = Create(desc, fd, static_cast<size_t>(wire.size), wire.id, /*foreign=*/true, &error);
  if (b == nullptr) {
    close(fd);
    return error != 0 ? error : -ENOMEM;
  }
  *out = ToApp(b);
  return 0;
}

void Register() {
  struct Entry {
    const char* name;
    void* fn;
  };
#define E(name, fn) {name, reinterpret_cast<void*>(&fn)}
  static const Entry kEntries[] = {
      E("AHardwareBuffer_allocate", Allocate),
      E("AHardwareBuffer_isSupported", IsSupported),
      E("AHardwareBuffer_acquire", AcquireSysv),
      E("AHardwareBuffer_release", ReleaseSysv),
      E("AHardwareBuffer_describe", Describe),
      E("AHardwareBuffer_lock", Lock),
      E("AHardwareBuffer_lockAndGetInfo", LockAndGetInfo),
      E("AHardwareBuffer_lockPlanes", LockPlanes),
      E("AHardwareBuffer_unlock", Unlock),
      E("AHardwareBuffer_getId", GetId),
      E("AHardwareBuffer_sendHandleToUnixSocket", SendHandle),
      E("AHardwareBuffer_recvHandleFromUnixSocket", RecvHandle),
  };
#undef E
  for (const Entry& e : kEntries) AddBionicSymbol(e.name, e.fn);
}

[[maybe_unused]] const bool g_registered = (Register(), true);

}  // namespace
}  // namespace gesher::bionic::ahb
