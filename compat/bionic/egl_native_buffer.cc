// gesher: Android's EGL for native buffers and fences, over ANGLE (ADR 0089).
//
// ANGLE's Direct3D 11 backend has no EGL_ANDROID_* buffer or fence
// extensions; Android apps (Chromium's GPU process) need them with
// AHardwareBuffer. They are implemented here, for Android's default display:
//  - EGL_ANDROID_get_native_client_buffer: eglGetNativeClientBufferANDROID
//    gives the buffer's ANativeWindowBuffer, 16 bytes into it, as on Android
//    (hardware_buffer.cc: code in apps relies on that layout);
//  - EGL_ANDROID_image_native_buffer: eglCreateImageKHR / eglCreateImage with
//    EGL_NATIVE_BUFFER_ANDROID give an EGLImage of the buffer (hardware_buffer.h:
//    an ANGLE image, EGL_ANGLE_image_d3d11_texture, of the Direct3D 11 texture
//    the GPU thread's device shares with ANGLE's, so whatever the app draws
//    into it through glEGLImageTargetTexture2DOES is the buffer's content);
//    each holds a reference to the buffer until it is destroyed;
//  - EGL_KHR_fence_sync, EGL_KHR_wait_sync and EGL_ANDROID_native_fence_sync:
//    ANGLE's Direct3D 11 display has no EGL fence syncs to build on, so a
//    fence records real DXGI GPU completion, without waiting for it. Local
//    server waits are already ordered on the shared immediate queue. Native
//    descriptor export waits outside ANGLE's mutex before exposing an immutable
//    completed event; an external pending GPU wait would stall unrelated EGL
//    contexts on that queue. See gpu_fence.cc and native_fence.cc.
//    Reusable syncs stay ANGLE's.
// eglQueryString(EGL_EXTENSIONS) adds the fence names for the default display,
// and the buffer names when ANGLE has what the images are made with
// (EGL_KHR_image_base, EGL_ANGLE_image_d3d11_texture, EGL_EXT_device_query).
// Everything else is ANGLE's.
// Errors are left in ANGLE's per-thread error state (an ANGLE call that fails
// with the same code), so eglGetError stays ANGLE's.
#include "gpu_interop.h"  // first: the EGL/GL prototype macros
#include "gpu_fence.h"
#include "egl_native_buffer.h"
#include "hardware_buffer.h"

#include "abi_types.h"

#include <gesher/egl_angle.h>

#include <errno.h>
#include <unistd.h>

#include <cstdint>
#include <array>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif
#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID 0x3145
#define EGL_SYNC_NATIVE_FENCE_SIGNALED_ANDROID 0x3146
#define EGL_NO_NATIVE_FENCE_FD_ANDROID -1
#endif
#ifndef EGL_PROTECTED_CONTENT_EXT
#define EGL_PROTECTED_CONTENT_EXT 0x32C0
#endif

namespace gesher::bionic::gl {

namespace {

// ANGLE's functions; the generated thunks abort the same way without ANGLE.
const gpu::Gl* G() {
  const gpu::Gl* g = gpu::Functions();
  if (g == nullptr) {
    std::fprintf(stderr, "gesher EGL: ANGLE is not loaded\n");
    std::fflush(stderr);
    std::abort();
  }
  return g;
}

EGLDisplay DefaultDisplay() { return static_cast<EGLDisplay>(gesher_egl_default_display()); }

// Leaves `error` in ANGLE's per-thread state through its private host bridge.
void SetError(EGLint error) {
  using Report = void(__stdcall*)(unsigned);
  static auto report = reinterpret_cast<Report>(gesher_egl_proc("gesherAngleEglErrorV1"));
  if (report != nullptr) { report(static_cast<unsigned>(error)); return; }
  // Older installed SDKs predate the precise host error hook.
  const gpu::Gl* g = gpu::Functions();
  EGLDisplay d = DefaultDisplay();
  if (g == nullptr || d == EGL_NO_DISPLAY) return;
  EGLint ignored = 0;
  switch (error) {
    case EGL_BAD_DISPLAY:
      g->eglQueryString(reinterpret_cast<EGLDisplay>(static_cast<uintptr_t>(0xbad0)), EGL_VENDOR);
      return;
    case EGL_BAD_CONTEXT:
      g->eglQueryContext(d, reinterpret_cast<EGLContext>(static_cast<uintptr_t>(0xbad0)), EGL_CONFIG_ID,
                         &ignored);
      return;
    case EGL_BAD_ATTRIBUTE:
    case EGL_BAD_MATCH: {
      EGLConfig config = nullptr;
      EGLint count = 0;
      const EGLint any[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
      if (g->eglChooseConfig(d, any, &config, 1, &count) == EGL_TRUE && count > 0) {
        if (error == EGL_BAD_ATTRIBUTE) {
          g->eglGetConfigAttrib(d, config, 0x7fff, &ignored);
          return;
        }
        // A texture format without a texture target.
        const EGLint bad[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA, EGL_NONE};
        if (EGLSurface s = g->eglCreatePbufferSurface(d, config, bad)) g->eglDestroySurface(d, s);
        return;
      }
      break;
    }
    default:
      break;
  }
  // EGL_BAD_PARAMETER, and what no call reproduces cleanly (EGL_BAD_ALLOC,
  // EGL_BAD_ACCESS): an unknown string name.
  g->eglQueryString(d, 0);
}

// --- native buffers ------------------------------------------------------------------------------

EGLClientBuffer BIONIC_SYSV GetNativeClientBuffer(const AHardwareBuffer* buffer) {
  ahb::Buffer* b = ahb::FromApp(buffer);
  if (b == nullptr) {
    SetError(EGL_BAD_PARAMETER);
    return nullptr;
  }
  return static_cast<EGLClientBuffer>(ahb::ClientBuffer(b));
}

EGLImageKHR NativeBufferImage(EGLDisplay dpy, EGLContext ctx, EGLClientBuffer buffer,
                              const std::vector<EGLint>& attribs) {
  if (ctx != EGL_NO_CONTEXT) {
    SetError(EGL_BAD_CONTEXT);
    return EGL_NO_IMAGE_KHR;
  }
  ahb::Buffer* b = ahb::FromClientBuffer(buffer);
  if (b == nullptr) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_NO_IMAGE_KHR;
  }
  for (size_t i = 0; i + 1 < attribs.size() && attribs[i] != EGL_NONE; i += 2) {
    switch (attribs[i]) {
      case EGL_IMAGE_PRESERVED_KHR:
        break;  // the contents are always preserved
      case EGL_PROTECTED_CONTENT_EXT:
        if (attribs[i + 1] != EGL_FALSE) {  // no buffer is protected
          SetError(EGL_BAD_ACCESS);
          return EGL_NO_IMAGE_KHR;
        }
        break;
      case EGL_GL_COLORSPACE_KHR:
        // The texture is sampled as stored (linear); there is no sRGB view
        // (EGL_EXT_image_gl_colorspace is not offered).
        if (attribs[i + 1] != EGL_GL_COLORSPACE_LINEAR_KHR) {
          SetError(EGL_BAD_PARAMETER);
          return EGL_NO_IMAGE_KHR;
        }
        break;
      default:
        SetError(EGL_BAD_PARAMETER);
        return EGL_NO_IMAGE_KHR;
    }
  }
  EGLint error = EGL_SUCCESS;
  EGLImageKHR image = ahb::AcquireAppImage(b, dpy, &error);
  if (image == EGL_NO_IMAGE_KHR) SetError(error);
  return image;
}

std::vector<EGLint> IntAttribs(const EGLint* attribs) {
  std::vector<EGLint> list;
  if (attribs != nullptr) {
    for (const EGLint* a = attribs; *a != EGL_NONE; a += 2) {
      list.push_back(a[0]);
      list.push_back(a[1]);
    }
  }
  list.push_back(EGL_NONE);
  return list;
}

std::vector<EGLint> IntAttribs(const EGLAttrib* attribs) {
  std::vector<EGLint> list;
  if (attribs != nullptr) {
    for (const EGLAttrib* a = attribs; *a != EGL_NONE; a += 2) {
      list.push_back(static_cast<EGLint>(a[0]));
      list.push_back(static_cast<EGLint>(a[1]));
    }
  }
  list.push_back(EGL_NONE);
  return list;
}

EGLImageKHR BIONIC_SYSV CreateImageKhr(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                                       EGLClientBuffer buffer, const EGLint* attribs) {
  if (target == EGL_NATIVE_BUFFER_ANDROID) return NativeBufferImage(dpy, ctx, buffer, IntAttribs(attribs));
  return G()->eglCreateImageKHR(dpy, ctx, target, buffer, attribs);
}

EGLImage BIONIC_SYSV CreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                                 EGLClientBuffer buffer, const EGLAttrib* attribs) {
  if (target == EGL_NATIVE_BUFFER_ANDROID) return NativeBufferImage(dpy, ctx, buffer, IntAttribs(attribs));
  auto angle = reinterpret_cast<decltype(&::eglCreateImage)>(gesher_egl_proc("eglCreateImage"));
  if (angle == nullptr) return G()->eglCreateImageKHR(dpy, ctx, target, buffer, IntAttribs(attribs).data());
  return angle(dpy, ctx, target, buffer, attribs);
}

EGLBoolean DestroyImageImpl(EGLDisplay dpy, EGLImageKHR image) {
  if (ahb::ReleaseAppImage(image) > 0) return EGL_TRUE;  // a buffer's image
  return G()->eglDestroyImageKHR(dpy, image);
}

EGLBoolean BIONIC_SYSV DestroyImageKhr(EGLDisplay dpy, EGLImageKHR image) {
  return DestroyImageImpl(dpy, image);
}

EGLBoolean BIONIC_SYSV DestroyImage(EGLDisplay dpy, EGLImage image) { return DestroyImageImpl(dpy, image); }

// --- fence syncs ---------------------------------------------------------------------------------
//
// EGL_KHR_fence_sync (EGL_SYNC_FENCE_KHR) and EGL_ANDROID_native_fence_sync
// (EGL_SYNC_NATIVE_FENCE_ANDROID) syncs are these objects; ANGLE's own
// (EGL_SYNC_REUSABLE_KHR) are passed to it. Submission and completion are
// distinct: creating a sync only submits the fence on supported drivers.

struct Sync {
  EGLenum type = 0;
  EGLint condition = EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR;
  EGLDisplay display = EGL_NO_DISPLAY;
  std::shared_ptr<NativeFence> fence;
  int fd = -1;  // owned only for imports; local native export is lazy
  std::atomic<unsigned> references{1};
  Sync* next = nullptr;  // registry bucket, under g_syncs_mutex
  ~Sync() {
    if (fd >= 0) close(fd);
  }
};

// Intrusive references avoid a throwing shared_ptr control-block allocation
// at the EGL ABI. Retiring a sync still preserves concurrent client waits.
class SyncRef {
 public:
  explicit SyncRef(Sync* sync = nullptr) : sync_(sync) {}
  ~SyncRef() { if (sync_ && sync_->references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete sync_; }
  SyncRef(const SyncRef&) = delete;
  SyncRef& operator=(const SyncRef&) = delete;
  Sync* get() const { return sync_; }
  Sync* operator->() const { return sync_; }
  explicit operator bool() const { return sync_ != nullptr; }
 private:
  Sync* sync_;
};

// A sync destroyed while another thread waits on it or reads it stays alive
// (with its fence fd open) until that call returns.
std::mutex g_syncs_mutex;
// The sync itself owns its hash link: no allocation/failure after acquiring
// an imported fd, no fixed application-visible limit on outstanding syncs.
std::array<Sync*, 256> g_syncs{};
Sync*& SyncBucket(void* sync) {
  return g_syncs[(reinterpret_cast<uintptr_t>(sync) >> 4) % g_syncs.size()];
}

SyncRef FindSync(void* sync) {
  std::lock_guard<std::mutex> lock(g_syncs_mutex);
  for (Sync* found = SyncBucket(sync); found; found = found->next) {
    if (found == sync) { found->references.fetch_add(1, std::memory_order_relaxed); return SyncRef(found); }
  }
  return SyncRef();
}

bool CurrentContext(EGLDisplay dpy) {
  const gpu::Gl* g = gpu::Functions();
  if (!g || g->eglGetCurrentContext() == EGL_NO_CONTEXT || g->eglGetCurrentDisplay() != dpy) {
    SetError(EGL_BAD_MATCH);
    return false;
  }
  return true;
}

bool SyncDisplay(EGLDisplay dpy, const Sync* sync) {
  if (dpy == EGL_NO_DISPLAY || dpy != DefaultDisplay()) { SetError(EGL_BAD_DISPLAY); return false; }
  if (dpy != sync->display) { SetError(EGL_BAD_PARAMETER); return false; }
  return true;
}

template <class Attribute>
EGLSyncKHR CreateFence(EGLDisplay dpy, EGLenum type, const Attribute* attribs) {
  int fd = EGL_NO_NATIVE_FENCE_FD_ANDROID;
  bool bad_attribute = false;
  for (const Attribute* a = attribs; a != nullptr && a[0] != EGL_NONE; a += 2) {
    if (type == EGL_SYNC_NATIVE_FENCE_ANDROID && a[0] == EGL_SYNC_NATIVE_FENCE_FD_ANDROID) {
      if (a[1] < INT_MIN || a[1] > INT_MAX) bad_attribute = true;
      else fd = static_cast<int>(a[1]);
    } else {
      bad_attribute = true;
    }
  }
  // The native extension transfers ownership on the call, even on failure.
  // Parse the whole list first so a preceding bad attribute cannot leak fd.
  struct ImportedFd {
    int fd;
    ~ImportedFd() { if (fd >= 0) close(fd); }
  } owned{fd};
  if (bad_attribute) { SetError(EGL_BAD_ATTRIBUTE); return EGL_NO_SYNC_KHR; }
  if (dpy == EGL_NO_DISPLAY || dpy != DefaultDisplay()) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_NO_SYNC_KHR;
  }
  if (fd < EGL_NO_NATIVE_FENCE_FD_ANDROID) {
    SetError(EGL_BAD_ATTRIBUTE);
    return EGL_NO_SYNC_KHR;
  }
  std::shared_ptr<NativeFence> fence;
  const bool imported = fd >= 0;
  if (!CurrentContext(dpy)) return EGL_NO_SYNC_KHR;
  if (fd == EGL_NO_NATIVE_FENCE_FD_ANDROID) {
    int error = EGL_SUCCESS;
    fence = gpu::CreateCurrentFence(dpy, &error);
    if (!fence) { SetError(error); return EGL_NO_SYNC_KHR; }
  } else {
    fence = LookupNativeFence(fd);
    if (!fence) { SetError(EGL_BAD_ATTRIBUTE); return EGL_NO_SYNC_KHR; }
  }
  auto* s = new (std::nothrow) Sync;
  if (!s) { SetError(EGL_BAD_ALLOC); return EGL_NO_SYNC_KHR; }
  s->type = type;
  s->condition = imported ? EGL_SYNC_NATIVE_FENCE_SIGNALED_ANDROID : EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR;
  s->display = dpy;
  s->fence = std::move(fence);
  s->fd = fd;  // given in the attributes: the implementation owns it from now on
  owned.fd = -1;
  std::lock_guard<std::mutex> lock(g_syncs_mutex);
  s->next = SyncBucket(s);
  SyncBucket(s) = s;
  return s;
}

bool OurSyncType(EGLenum type) { return type == EGL_SYNC_NATIVE_FENCE_ANDROID || type == EGL_SYNC_FENCE_KHR; }

EGLSyncKHR BIONIC_SYSV CreateSyncKhr(EGLDisplay dpy, EGLenum type, const EGLint* attribs) {
  if (OurSyncType(type)) return CreateFence(dpy, type, attribs);
  return G()->eglCreateSyncKHR(dpy, type, attribs);
}

EGLSync BIONIC_SYSV CreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib* attribs) {
  if (OurSyncType(type)) return CreateFence(dpy, type, attribs);
  auto angle = reinterpret_cast<decltype(&::eglCreateSync)>(gesher_egl_proc("eglCreateSync"));
  if (angle == nullptr) return G()->eglCreateSyncKHR(dpy, type, IntAttribs(attribs).data());
  return angle(dpy, type, attribs);
}

EGLBoolean DestroySyncImpl(EGLDisplay dpy, void* sync) {
  Sync* retired = nullptr;
  bool wrong_display = false;
  {
    std::lock_guard<std::mutex> lock(g_syncs_mutex);
    for (Sync** link = &SyncBucket(sync); *link; link = &(*link)->next) {
      Sync* found = *link;
      if (found != sync) continue;
      wrong_display = dpy != found->display;
      if (!wrong_display) { retired = found; *link = found->next; found->next = nullptr; }
      break;
    }
  }
  if (wrong_display) { SetError(EGL_BAD_DISPLAY); return EGL_FALSE; }
  SyncRef owner(retired);
  if (!owner) return G()->eglDestroySyncKHR(dpy, sync);
  return EGL_TRUE;
}

EGLBoolean BIONIC_SYSV DestroySyncKhr(EGLDisplay dpy, EGLSyncKHR sync) { return DestroySyncImpl(dpy, sync); }
EGLBoolean BIONIC_SYSV DestroySync(EGLDisplay dpy, EGLSync sync) { return DestroySyncImpl(dpy, sync); }

EGLint ClientWaitImpl(EGLDisplay dpy, void* sync, EGLint flags, uint64_t timeout) {
  SyncRef s = FindSync(sync);
  if (!s) return G()->eglClientWaitSyncKHR(dpy, sync, flags, timeout);
  if (!SyncDisplay(dpy, s.get())) return EGL_FALSE;
  if ((flags & ~EGL_SYNC_FLUSH_COMMANDS_BIT_KHR) != 0) { SetError(EGL_BAD_PARAMETER); return EGL_FALSE; }
  // The flag flushes the CALLING context, including work after this fence was
  // made (or on another context). Creation's earlier flush cannot replace it.
  if ((flags & EGL_SYNC_FLUSH_COMMANDS_BIT_KHR) && NativeFenceStatus(s->fence) == FenceStatus::kPending &&
      G()->eglGetCurrentContext() != EGL_NO_CONTEXT) {
    static auto flush = reinterpret_cast<decltype(&::glFlush)>(gesher_egl_proc("glFlush"));
    flush();
  }
  // Even an infinite client wait holds neither ANGLE's mutex nor our registry.
  FenceStatus status = WaitNativeFence(s->fence, timeout);
  if (status == FenceStatus::kFailed) { SetError(EGL_CONTEXT_LOST); return EGL_FALSE; }
  return status == FenceStatus::kSignaled ? EGL_CONDITION_SATISFIED_KHR : EGL_TIMEOUT_EXPIRED_KHR;
}

EGLint BIONIC_SYSV ClientWaitSyncKhr(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout) {
  return ClientWaitImpl(dpy, sync, flags, timeout);
}

EGLint BIONIC_SYSV ClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout) {
  return ClientWaitImpl(dpy, sync, flags, timeout);
}

EGLint WaitImpl(EGLDisplay dpy, void* sync, EGLint flags) {
  SyncRef s = FindSync(sync);
  if (!s) return G()->eglWaitSyncKHR(dpy, sync, flags);
  if (!SyncDisplay(dpy, s.get())) return EGL_FALSE;
  if (flags != 0) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  if (!CurrentContext(dpy)) return EGL_FALSE;
  int error = EGL_SUCCESS;
  if (!gpu::WaitCurrentFence(s->fence, &error)) { SetError(error); return EGL_FALSE; }
  return EGL_TRUE;
}

EGLint BIONIC_SYSV WaitSyncKhr(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags) { return WaitImpl(dpy, sync, flags); }

EGLBoolean BIONIC_SYSV WaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags) {
  return WaitImpl(dpy, sync, flags) == EGL_TRUE ? EGL_TRUE : EGL_FALSE;
}

bool SyncAttrib(const Sync* s, EGLint attribute, EGLint* value) {
  switch (attribute) {
    case EGL_SYNC_TYPE_KHR:
      *value = static_cast<EGLint>(s->type);
      return true;
    case EGL_SYNC_CONDITION_KHR:
      *value = s->condition;
      return true;
    case EGL_SYNC_STATUS_KHR: {
      FenceStatus status = NativeFenceStatus(s->fence);
      if (status == FenceStatus::kFailed) { SetError(EGL_CONTEXT_LOST); return false; }
      *value = status == FenceStatus::kSignaled ? EGL_SIGNALED_KHR : EGL_UNSIGNALED_KHR;
      return true;
    }
    default:
      SetError(EGL_BAD_ATTRIBUTE);
      return false;
  }
}

EGLBoolean BIONIC_SYSV GetSyncAttribKhr(EGLDisplay dpy, EGLSyncKHR sync, EGLint attribute, EGLint* value) {
  SyncRef s = FindSync(sync);
  if (!s) return G()->eglGetSyncAttribKHR(dpy, sync, attribute, value);
  if (!SyncDisplay(dpy, s.get())) return EGL_FALSE;
  if (value == nullptr) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  return SyncAttrib(s.get(), attribute, value) ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean BIONIC_SYSV GetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib* value) {
  SyncRef s = FindSync(sync);
  if (!s) {
    auto angle = reinterpret_cast<decltype(&::eglGetSyncAttrib)>(gesher_egl_proc("eglGetSyncAttrib"));
    if (angle != nullptr) return angle(dpy, sync, attribute, value);
    EGLint v = 0;
    EGLBoolean ok = G()->eglGetSyncAttribKHR(dpy, sync, attribute, &v);
    if (ok == EGL_TRUE && value != nullptr) *value = v;
    return ok;
  }
  if (!SyncDisplay(dpy, s.get())) return EGL_FALSE;
  if (value == nullptr) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  EGLint v = 0;
  if (!SyncAttrib(s.get(), attribute, &v)) return EGL_FALSE;
  *value = v;
  return EGL_TRUE;
}

EGLBoolean BIONIC_SYSV SignalSyncKhr(EGLDisplay dpy, EGLSyncKHR sync, EGLenum mode) {
  if (FindSync(sync)) {
    SetError(EGL_BAD_MATCH);  // only reusable syncs are signaled by hand
    return EGL_FALSE;
  }
  auto angle = reinterpret_cast<decltype(&::eglSignalSyncKHR)>(gesher_egl_proc("eglSignalSyncKHR"));
  if (angle == nullptr) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  return angle(dpy, sync, mode);
}

EGLint BIONIC_SYSV DupNativeFenceFd(EGLDisplay dpy, EGLSyncKHR sync) {
  SyncRef s = FindSync(sync);
  if (!s || s->type != EGL_SYNC_NATIVE_FENCE_ANDROID) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_NO_NATIVE_FENCE_FD_ANDROID;
  }
  if (!SyncDisplay(dpy, s.get())) return EGL_NO_NATIVE_FENCE_FD_ANDROID;
  // The native event exists and has been submitted at creation. The POSIX
  // descriptor is materialized only when requested; native fd attributes are
  // deliberately not queryable by EGL (ANDROID_native_fence_sync issue 3).
  int fd = s->fd >= 0 ? dup(s->fd) : ExportNativeFence(s->fence);
  if (fd < 0) SetError(errno == EIO ? EGL_CONTEXT_LOST : EGL_BAD_ALLOC);
  return fd;
}

// --- extensions ----------------------------------------------------------------------------------

bool HasExtension(const char* list, const char* name) {
  const size_t n = std::strlen(name);
  for (const char* p = list; (p = std::strstr(p, name)) != nullptr; p += n) {
    if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
  }
  return false;
}

const char* BIONIC_SYSV QueryString(EGLDisplay dpy, EGLint name) {
  const gpu::Gl* g = G();
  const char* s = g->eglQueryString(dpy, name);
  if (name != EGL_EXTENSIONS || s == nullptr || dpy == EGL_NO_DISPLAY || dpy != DefaultDisplay()) return s;
  static std::mutex mutex;
  static std::string* extended = nullptr;
  std::lock_guard<std::mutex> lock(mutex);
  if (extended == nullptr) {
    std::string list = s;
    auto add = [&list, s](const char* name) {
      if (HasExtension(s, name)) return;
      if (!list.empty() && list.back() != ' ') list += ' ';
      list += name;
    };
    // EGL_ANDROID_native_fence_sync is written against EGL_KHR_fence_sync,
    // and code built on Android's EGL (ANGLE's GL back end) loads the sync
    // entry points only with EGL_KHR_fence_sync and EGL_KHR_wait_sync named.
    add("EGL_KHR_fence_sync");
    add("EGL_KHR_wait_sync");
    add("EGL_ANDROID_native_fence_sync");
    // The images: ANGLE images of Direct3D 11 textures shared with ANGLE's
    // device, which EGL_EXT_device_query gives (gpu_interop.h). Decided from
    // ANGLE's extensions, so a process that never uses a buffer does not
    // start the GPU thread for this query.
    const char* client = g->eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (HasExtension(s, "EGL_KHR_image_base") && HasExtension(s, "EGL_ANGLE_image_d3d11_texture") &&
        client != nullptr && HasExtension(client, "EGL_EXT_device_query")) {
      add("EGL_ANDROID_get_native_client_buffer");
      add("EGL_ANDROID_image_native_buffer");
    }
    extended = new std::string(std::move(list));
  }
  return extended->c_str();
}

}  // namespace

void RegisterNativeBufferEgl(void (*add)(const char* name, void* fn)) {
#define E(name, fn) add(name, reinterpret_cast<void*>(&fn))
  E("eglGetNativeClientBufferANDROID", GetNativeClientBuffer);
  E("eglCreateImageKHR", CreateImageKhr);
  E("eglCreateImage", CreateImage);
  E("eglDestroyImageKHR", DestroyImageKhr);
  E("eglDestroyImage", DestroyImage);
  E("eglCreateSyncKHR", CreateSyncKhr);
  E("eglCreateSync", CreateSync);
  E("eglDestroySyncKHR", DestroySyncKhr);
  E("eglDestroySync", DestroySync);
  E("eglClientWaitSyncKHR", ClientWaitSyncKhr);
  E("eglClientWaitSync", ClientWaitSync);
  E("eglWaitSyncKHR", WaitSyncKhr);
  E("eglWaitSync", WaitSync);
  E("eglGetSyncAttribKHR", GetSyncAttribKhr);
  E("eglGetSyncAttrib", GetSyncAttrib);
  E("eglSignalSyncKHR", SignalSyncKhr);
  E("eglDupNativeFenceFDANDROID", DupNativeFenceFd);
  E("eglQueryString", QueryString);
#undef E
}

}  // namespace gesher::bionic::gl
