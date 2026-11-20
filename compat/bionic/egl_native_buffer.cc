// gesher: Android's EGL for native buffers and fences, over ANGLE (ADR 0089).
//
// ANGLE's Direct3D 11 backend has no EGL_ANDROID_* buffer or fence
// extensions; Android apps (Chromium's GPU process) need them with
// AHardwareBuffer. They are implemented here, for Android's default display:
//  - EGL_ANDROID_get_native_client_buffer: eglGetNativeClientBufferANDROID
//    gives the buffer's ANativeWindowBuffer, 16 bytes into it, as on Android
//    (hardware_buffer.cc: code in apps relies on that layout);
//  - EGL_ANDROID_image_native_buffer: eglCreateImageKHR / eglCreateImage with
//    EGL_NATIVE_BUFFER_ANDROID give the buffer's EGLImage (hardware_buffer.h:
//    an ANGLE image of the GPU thread's texture, so whatever the app draws
//    into it through glEGLImageTargetTexture2DOES is the buffer's content);
//    creating it twice gives the same image, released after as many destroys;
//  - EGL_ANDROID_native_fence_sync: ANGLE's Direct3D 11 display has no EGL
//    fence syncs (EGL_KHR_fence_sync) to build on, so a native fence sync made
//    without an fd waits, when it is made, for the current context's commands
//    to finish (glFinish) and then holds an fd that is signaled (a /dev/null
//    descriptor: polls readable, crosses processes, reports no sync_file
//    details): the fd never signals before the GPU work before the fence is
//    done. One made from an fd (from this process or another) is waited for by
//    polling it: eglWaitSyncKHR waits on the CPU before later commands are
//    issued, which keeps the order the server wait promises. The cost is
//    CPU/GPU overlap: making a fence blocks until the GPU has caught up.
// eglQueryString(EGL_EXTENSIONS) adds the three names for the default display
// when ANGLE has what the images are built on (EGL_KHR_image_base,
// EGL_KHR_gl_texture_2D_image). Everything else is ANGLE's.
// Errors are left in ANGLE's per-thread error state (an ANGLE call that fails
// with the same code), so eglGetError stays ANGLE's.
#include "gpu_interop.h"  // first: the EGL/GL prototype macros
#include "egl_native_buffer.h"
#include "hardware_buffer.h"

#include "abi_types.h"

#include <gesher/egl_angle.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// Leaves `error` in ANGLE's per-thread state with a call that fails with it.
void SetError(EGLint error) {
  const gpu::Gl* g = G();
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
  const int released = ahb::ReleaseAppImage(image);
  if (released > 0) return EGL_TRUE;
  if (released < 0) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  return G()->eglDestroyImageKHR(dpy, image);
}

EGLBoolean BIONIC_SYSV DestroyImageKhr(EGLDisplay dpy, EGLImageKHR image) {
  return DestroyImageImpl(dpy, image);
}

EGLBoolean BIONIC_SYSV DestroyImage(EGLDisplay dpy, EGLImage image) { return DestroyImageImpl(dpy, image); }

// --- native fences -------------------------------------------------------------------------------

struct NativeFence {
  EGLDisplay display = EGL_NO_DISPLAY;
  int fd = -1;  // signaled when the fence is
};

std::mutex g_fences_mutex;
std::unordered_set<NativeFence*>& Fences() {
  static auto* set = new std::unordered_set<NativeFence*>();
  return *set;
}

NativeFence* FindFence(void* sync) {
  auto* f = static_cast<NativeFence*>(sync);
  std::lock_guard<std::mutex> lock(g_fences_mutex);
  return Fences().count(f) != 0 ? f : nullptr;
}

int SignaledFenceFd() { return open("/dev/null", O_RDONLY | O_CLOEXEC); }

// Waits for `fd` up to `timeout_ns` (EGL_FOREVER_KHR: no limit); true when signaled.
bool PollFence(int fd, uint64_t timeout_ns) {
  int ms = timeout_ns == EGL_FOREVER_KHR ? -1
           : static_cast<int>(std::min<uint64_t>((timeout_ns + 999999) / 1000000, 0x7fffffff));
  pollfd p{fd, POLLIN, 0};
  for (;;) {
    int n = poll(&p, 1, ms);
    if (n > 0) return (p.revents & (POLLERR | POLLNVAL)) == 0;
    if (n == 0) return false;
    if (errno != EINTR) return false;
  }
}

EGLSyncKHR CreateNativeFence(EGLDisplay dpy, const std::vector<EGLint>& attribs) {
  int fd = EGL_NO_NATIVE_FENCE_FD_ANDROID;
  for (size_t i = 0; i + 1 < attribs.size() && attribs[i] != EGL_NONE; i += 2) {
    if (attribs[i] == EGL_SYNC_NATIVE_FENCE_FD_ANDROID) {
      fd = attribs[i + 1];
    } else {
      SetError(EGL_BAD_ATTRIBUTE);
      return EGL_NO_SYNC_KHR;
    }
  }
  const gpu::Gl* g = G();
  if (fd < 0) {
    // A fence after the current context's commands (EGL_BAD_MATCH without a
    // context current on this display): they are waited for now, so the fd
    // is signaled from the start and never before the work is done.
    if (g->eglGetCurrentContext() == EGL_NO_CONTEXT || g->eglGetCurrentDisplay() != dpy) {
      SetError(EGL_BAD_MATCH);
      return EGL_NO_SYNC_KHR;
    }
    static auto finish = reinterpret_cast<decltype(&::glFinish)>(gesher_egl_proc("glFinish"));
    finish();
    fd = SignaledFenceFd();
    if (fd < 0) {
      SetError(EGL_BAD_ALLOC);
      return EGL_NO_SYNC_KHR;
    }
  }
  auto* f = new NativeFence();
  f->display = dpy;
  f->fd = fd;  // given in the attributes: the implementation owns it from now on
  std::lock_guard<std::mutex> lock(g_fences_mutex);
  Fences().insert(f);
  return f;
}

EGLSyncKHR BIONIC_SYSV CreateSyncKhr(EGLDisplay dpy, EGLenum type, const EGLint* attribs) {
  if (type == EGL_SYNC_NATIVE_FENCE_ANDROID) return CreateNativeFence(dpy, IntAttribs(attribs));
  return G()->eglCreateSyncKHR(dpy, type, attribs);
}

EGLSync BIONIC_SYSV CreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib* attribs) {
  if (type == EGL_SYNC_NATIVE_FENCE_ANDROID) return CreateNativeFence(dpy, IntAttribs(attribs));
  auto angle = reinterpret_cast<decltype(&::eglCreateSync)>(gesher_egl_proc("eglCreateSync"));
  if (angle == nullptr) return G()->eglCreateSyncKHR(dpy, type, IntAttribs(attribs).data());
  return angle(dpy, type, attribs);
}

EGLBoolean DestroySyncImpl(EGLDisplay dpy, void* sync) {
  NativeFence* f = FindFence(sync);
  if (f == nullptr) return G()->eglDestroySyncKHR(dpy, sync);
  {
    std::lock_guard<std::mutex> lock(g_fences_mutex);
    Fences().erase(f);
  }
  if (f->fd >= 0) close(f->fd);
  delete f;
  return EGL_TRUE;
}

EGLBoolean BIONIC_SYSV DestroySyncKhr(EGLDisplay dpy, EGLSyncKHR sync) { return DestroySyncImpl(dpy, sync); }
EGLBoolean BIONIC_SYSV DestroySync(EGLDisplay dpy, EGLSync sync) { return DestroySyncImpl(dpy, sync); }

EGLint ClientWaitImpl(EGLDisplay dpy, void* sync, EGLint flags, uint64_t timeout) {
  NativeFence* f = FindFence(sync);
  if (f == nullptr) return G()->eglClientWaitSyncKHR(dpy, sync, flags, timeout);
  return PollFence(f->fd, timeout) ? EGL_CONDITION_SATISFIED_KHR : EGL_TIMEOUT_EXPIRED_KHR;
}

EGLint BIONIC_SYSV ClientWaitSyncKhr(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout) {
  return ClientWaitImpl(dpy, sync, flags, timeout);
}

EGLint BIONIC_SYSV ClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout) {
  return ClientWaitImpl(dpy, sync, flags, timeout);
}

EGLint WaitImpl(EGLDisplay dpy, void* sync, EGLint flags) {
  NativeFence* f = FindFence(sync);
  if (f == nullptr) return G()->eglWaitSyncKHR(dpy, sync, flags);
  if (flags != 0) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  // The commands after the wait are issued once the fence has signaled.
  PollFence(f->fd, EGL_FOREVER_KHR);
  return EGL_TRUE;
}

EGLint BIONIC_SYSV WaitSyncKhr(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags) { return WaitImpl(dpy, sync, flags); }

EGLBoolean BIONIC_SYSV WaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags) {
  return WaitImpl(dpy, sync, flags) == EGL_TRUE ? EGL_TRUE : EGL_FALSE;
}

bool FenceAttrib(NativeFence* f, EGLint attribute, EGLint* value) {
  switch (attribute) {
    case EGL_SYNC_TYPE_KHR:
      *value = EGL_SYNC_NATIVE_FENCE_ANDROID;
      return true;
    case EGL_SYNC_CONDITION_KHR:
      *value = EGL_SYNC_NATIVE_FENCE_SIGNALED_ANDROID;
      return true;
    case EGL_SYNC_STATUS_KHR:
      *value = PollFence(f->fd, 0) ? EGL_SIGNALED_KHR : EGL_UNSIGNALED_KHR;
      return true;
    default:
      SetError(EGL_BAD_ATTRIBUTE);
      return false;
  }
}

EGLBoolean BIONIC_SYSV GetSyncAttribKhr(EGLDisplay dpy, EGLSyncKHR sync, EGLint attribute, EGLint* value) {
  NativeFence* f = FindFence(sync);
  if (f == nullptr) return G()->eglGetSyncAttribKHR(dpy, sync, attribute, value);
  if (value == nullptr) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  return FenceAttrib(f, attribute, value) ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean BIONIC_SYSV GetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib* value) {
  NativeFence* f = FindFence(sync);
  if (f == nullptr) {
    auto angle = reinterpret_cast<decltype(&::eglGetSyncAttrib)>(gesher_egl_proc("eglGetSyncAttrib"));
    if (angle != nullptr) return angle(dpy, sync, attribute, value);
    EGLint v = 0;
    EGLBoolean ok = G()->eglGetSyncAttribKHR(dpy, sync, attribute, &v);
    if (ok == EGL_TRUE && value != nullptr) *value = v;
    return ok;
  }
  if (value == nullptr) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  EGLint v = 0;
  if (!FenceAttrib(f, attribute, &v)) return EGL_FALSE;
  *value = v;
  return EGL_TRUE;
}

EGLBoolean BIONIC_SYSV SignalSyncKhr(EGLDisplay dpy, EGLSyncKHR sync, EGLenum mode) {
  if (FindFence(sync) != nullptr) {
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

EGLint BIONIC_SYSV DupNativeFenceFd(EGLDisplay /*dpy*/, EGLSyncKHR sync) {
  NativeFence* f = FindFence(sync);
  if (f == nullptr) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_NO_NATIVE_FENCE_FD_ANDROID;
  }
  int fd = dup(f->fd);
  if (fd < 0) SetError(EGL_BAD_ALLOC);
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
    if (HasExtension(s, "EGL_KHR_image_base") && HasExtension(s, "EGL_KHR_gl_texture_2D_image")) {
      for (const char* add : {"EGL_ANDROID_get_native_client_buffer", "EGL_ANDROID_image_native_buffer",
                              "EGL_ANDROID_native_fence_sync"}) {
        if (!HasExtension(s, add)) {
          if (!list.empty() && list.back() != ' ') list += ' ';
          list += add;
        }
      }
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
