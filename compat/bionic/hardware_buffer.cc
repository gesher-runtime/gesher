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
// The buffer's GPU form is a texture of the GPU thread and an EGLImage made
// from it (hardware_buffer.h explains when memory and texture are synchronized).
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

#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT 0x140B
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
  int bytes;              // per pixel (BLOB: per byte)
  bool gpu;               // has a texture form
  GLenum internal;        // texture storage
  GLenum upload_format;   // glTexSubImage2D source (after conversion, see Upload)
  GLenum upload_type;
  bool opaque;            // no alpha channel
};

const Format kFormats[] = {
    {AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 4, true, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, false},
    // Stored as RGBA8 in memory, sampled with alpha 1 (an RGB8 texture).
    {AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 4, true, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, true},
    {AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM, 3, true, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, true},
    {AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM, 2, true, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, true},
    {AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT, 8, true, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, false},
    {AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM, 4, true, GL_RGB10_A2, GL_RGBA,
     GL_UNSIGNED_INT_2_10_10_10_REV, false},
    {AHARDWAREBUFFER_FORMAT_R8_UNORM, 1, true, GL_R8, GL_RED, GL_UNSIGNED_BYTE, true},
    {AHARDWAREBUFFER_FORMAT_BLOB, 1, false, 0, 0, 0, true},
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
    if (f->internal == GL_RGBA16F && (desc->usage & AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER) != 0 &&
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
  GLuint texture = 0;
  GLuint fbo = 0;
  EGLImageKHR image = EGL_NO_IMAGE_KHR;
  EGLDisplay display = EGL_NO_DISPLAY;
  int app_images = 0;  // app references to `image`
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
  GpuState gpu;               // GPU thread only

  size_t row_bytes() const { return static_cast<size_t>(desc.stride) * format->bytes; }
};

namespace {

std::mutex g_registry_mutex;
std::unordered_set<Buffer*>& Registry() {
  static auto* set = new std::unordered_set<Buffer*>();
  return *set;
}

std::mutex g_images_mutex;
std::unordered_map<EGLImageKHR, Buffer*>& Images() {
  static auto* map = new std::unordered_map<EGLImageKHR, Buffer*>();
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

// IEEE half <-> float, for RGBA16F read-backs.
uint16_t FloatToHalf(float value) {
  uint32_t f;
  std::memcpy(&f, &value, 4);
  const uint32_t sign = (f >> 16) & 0x8000u;
  const int32_t exponent = static_cast<int32_t>((f >> 23) & 0xff) - 127 + 15;
  uint32_t mantissa = f & 0x7fffffu;
  if (((f >> 23) & 0xff) == 0xff) return static_cast<uint16_t>(sign | 0x7c00u | (mantissa ? 0x200u : 0));
  if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
  if (exponent <= 0) {
    if (exponent < -10) return static_cast<uint16_t>(sign);
    mantissa |= 0x800000u;
    const uint32_t shift = static_cast<uint32_t>(14 - exponent);
    uint32_t half = mantissa >> shift;
    if ((mantissa >> (shift - 1)) & 1u) half += 1;  // round half up
    return static_cast<uint16_t>(sign | half);
  }
  uint32_t half = sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
  if (mantissa & 0x1000u) half += 1;  // round to nearest (carries into the exponent correctly)
  return static_cast<uint16_t>(half);
}

// --- on the GPU thread ----------------------------------------------------------------------------

const gpu::Gl& G() { return gpu::Thread::Get()->gl(); }

// Uploads rect `r` of the buffer's memory into its texture.
void UploadRect(Buffer* b, const ARect& r) {
  if (b->gpu.texture == 0 || Empty(r)) return;
  const gpu::Gl& g = G();
  const Format* f = b->format;
  const int w = r.right - r.left, h = r.bottom - r.top;
  g.glBindTexture(GL_TEXTURE_2D, b->gpu.texture);
  if (f->format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM) {
    // RGBX in memory, RGB to the RGB8 texture.
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
      const uint8_t* s = b->data + (static_cast<size_t>(r.top + y) * b->desc.stride + r.left) * 4;
      uint8_t* d = &rgb[static_cast<size_t>(y) * w * 3];
      for (int x = 0; x < w; ++x) {
        d[x * 3 + 0] = s[x * 4 + 0];
        d[x * 3 + 1] = s[x * 4 + 1];
        d[x * 3 + 2] = s[x * 4 + 2];
      }
    }
    g.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    g.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    g.glTexSubImage2D(GL_TEXTURE_2D, 0, r.left, r.top, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
  } else {
    g.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    g.glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(b->desc.stride));
    const uint8_t* src = b->data + (static_cast<size_t>(r.top) * b->desc.stride + r.left) * f->bytes;
    g.glTexSubImage2D(GL_TEXTURE_2D, 0, r.left, r.top, w, h, f->upload_format, f->upload_type, src);
    g.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  }
  g.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  g.glBindTexture(GL_TEXTURE_2D, 0);
}

// Creates the texture (with the memory's contents) if the buffer has none.
bool EnsureTexture(Buffer* b) {
  if (b->gpu.texture != 0) return true;
  if (!b->format->gpu) return false;
  const gpu::Gl& g = G();
  GLuint texture = 0;
  g.glGenTextures(1, &texture);
  g.glBindTexture(GL_TEXTURE_2D, texture);
  g.glTexStorage2D(GL_TEXTURE_2D, 1, b->format->internal, static_cast<GLsizei>(b->desc.width),
                   static_cast<GLsizei>(b->desc.height));
  g.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  g.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  g.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  g.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  g.glBindTexture(GL_TEXTURE_2D, 0);
  GLenum error = g.glGetError();
  if (error != GL_NO_ERROR) {
    std::fprintf(stderr, "gesher AHardwareBuffer: no %ux%u texture (format 0x%x): GL error 0x%x\n",
                 b->desc.width, b->desc.height, b->desc.format, error);
    g.glDeleteTextures(1, &texture);
    return false;
  }
  b->gpu.texture = texture;
  UploadRect(b, Clip(b, nullptr));
  {
    std::lock_guard<std::mutex> lock(b->mutex);
    b->memory_newer = false;
    b->has_texture = true;
  }
  return true;
}

// Reads rect `r` of the texture back into memory.
void ReadbackRect(Buffer* b, const ARect& r) {
  if (b->gpu.texture == 0 || Empty(r)) return;
  const gpu::Gl& g = G();
  const Format* f = b->format;
  if (b->gpu.fbo == 0) g.glGenFramebuffers(1, &b->gpu.fbo);
  g.glBindFramebuffer(GL_FRAMEBUFFER, b->gpu.fbo);
  g.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b->gpu.texture, 0);
  if (g.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    static std::atomic<bool> said{false};
    LogOnce(&said, "a buffer's texture cannot be read back (incomplete framebuffer): CPU reads see "
                   "the last CPU contents");
    g.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return;
  }
  const int w = r.right - r.left, h = r.bottom - r.top;
  g.glPixelStorei(GL_PACK_ALIGNMENT, 1);
  if (f->format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ||
      f->format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM ||
      f->format == AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM) {
    // Straight into memory: rows of the texture are rows of the buffer.
    g.glPixelStorei(GL_PACK_ROW_LENGTH, static_cast<GLint>(b->desc.stride));
    uint8_t* dst = b->data + (static_cast<size_t>(r.top) * b->desc.stride + r.left) * 4;
    const GLenum type = f->format == AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM
                            ? GL_UNSIGNED_INT_2_10_10_10_REV : GL_UNSIGNED_BYTE;
    g.glReadPixels(r.left, r.top, w, h, GL_RGBA, type, dst);
    g.glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  } else if (f->format == AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT) {
    std::vector<float> px(static_cast<size_t>(w) * h * 4);
    g.glReadPixels(r.left, r.top, w, h, GL_RGBA, GL_FLOAT, px.data());
    for (int y = 0; y < h; ++y) {
      auto* d = reinterpret_cast<uint16_t*>(b->data + (static_cast<size_t>(r.top + y) * b->desc.stride +
                                                       r.left) * 8);
      for (int i = 0; i < w * 4; ++i) d[i] = FloatToHalf(px[static_cast<size_t>(y) * w * 4 + i]);
    }
  } else {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    g.glReadPixels(r.left, r.top, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < h; ++y) {
      const uint8_t* s = &px[static_cast<size_t>(y) * w * 4];
      uint8_t* d = b->data + (static_cast<size_t>(r.top + y) * b->desc.stride + r.left) * f->bytes;
      for (int x = 0; x < w; ++x) {
        const uint8_t* p = s + x * 4;
        switch (f->format) {
          case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
            d[x * 3 + 0] = p[0];
            d[x * 3 + 1] = p[1];
            d[x * 3 + 2] = p[2];
            break;
          case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM: {
            const uint16_t v = static_cast<uint16_t>(((p[0] * 31 + 127) / 255) << 11 |
                                                     ((p[1] * 63 + 127) / 255) << 5 |
                                                     ((p[2] * 31 + 127) / 255));
            std::memcpy(d + x * 2, &v, 2);
            break;
          }
          case AHARDWAREBUFFER_FORMAT_R8_UNORM:
            d[x] = p[0];
            break;
          default:
            break;
        }
      }
    }
  }
  g.glPixelStorei(GL_PACK_ALIGNMENT, 4);
  g.glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void FreeGpu(Buffer* b) {
  const gpu::Gl& g = G();
  if (b->gpu.image != EGL_NO_IMAGE_KHR) {
    {
      std::lock_guard<std::mutex> lock(g_images_mutex);
      Images().erase(b->gpu.image);
    }
    g.eglDestroyImageKHR(b->gpu.display, b->gpu.image);
  }
  if (b->gpu.fbo != 0) g.glDeleteFramebuffers(1, &b->gpu.fbo);
  if (b->gpu.texture != 0) g.glDeleteTextures(1, &b->gpu.texture);
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
      it = it->second == b ? Images().erase(it) : std::next(it);
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
  EGLImageKHR image = EGL_NO_IMAGE_KHR;
  thread->Run([&] {
    if (!EnsureTexture(b)) {
      *egl_error = EGL_BAD_ALLOC;
      return;
    }
    if (b->gpu.image == EGL_NO_IMAGE_KHR) {
      const gpu::Gl& g = thread->gl();
      const EGLint attribs[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
      b->gpu.image = g.eglCreateImageKHR(thread->display(), thread->context(), EGL_GL_TEXTURE_2D_KHR,
                                         reinterpret_cast<EGLClientBuffer>(
                                             static_cast<uintptr_t>(b->gpu.texture)),
                                         attribs);
      if (b->gpu.image == EGL_NO_IMAGE_KHR) {
        *egl_error = g.eglGetError();
        std::fprintf(stderr, "gesher AHardwareBuffer: no EGLImage for a %ux%u texture: 0x%x\n",
                     b->desc.width, b->desc.height, *egl_error);
        return;
      }
      b->gpu.display = thread->display();
    }
    b->gpu.app_images++;
    image = b->gpu.image;
  });
  if (image == EGL_NO_IMAGE_KHR) return image;
  Acquire(b);  // the app's image holds the buffer, as on Android
  {
    std::lock_guard<std::mutex> lock(b->mutex);
    b->gpu_written = true;  // the app may render into it from now on
  }
  std::lock_guard<std::mutex> lock(g_images_mutex);
  Images()[image] = b;  // until the buffer goes (FreeGpu)
  return image;
}

int ReleaseAppImage(EGLImageKHR image) {
  Buffer* b = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_images_mutex);
    auto it = Images().find(image);
    if (it == Images().end()) return 0;
    b = it->second;
    // Held for the call: the buffer's GPU form (and this entry) go with it.
    Acquire(b);
  }
  bool released = false;
  gpu::Thread::Get()->Run([&] {
    if (b->gpu.app_images > 0) {
      b->gpu.app_images--;
      released = true;
    }
  });
  if (released) Release(b);  // the app image's reference
  Release(b);
  return released ? 1 : -1;
}

GLuint CompositorTexture(Buffer* b) {
  if (!b->format->gpu) return 0;
  if (!EnsureTexture(b)) return 0;
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
