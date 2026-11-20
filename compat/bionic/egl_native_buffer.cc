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
//    fence sync (EGL_SYNC_FENCE_KHR, or EGL_SYNC_NATIVE_FENCE_ANDROID without
//    an fd) waits, when it is made, for the current context's commands to
//    finish (glFinish) and is signaled from then on; a native one holds an fd
//    that is signaled (a /dev/null descriptor: polls readable, crosses
//    processes, reports no sync_file details). Neither signals before the GPU
//    work before it is done. One made from an fd (from this process or
//    another) is waited for by polling it: eglWaitSyncKHR waits on the CPU
//    before later commands are issued, which keeps the order the server wait
//    promises. The cost is CPU/GPU overlap: making a fence blocks until the
//    GPU has caught up. Reusable syncs stay ANGLE's.
// eglQueryString(EGL_EXTENSIONS) adds the fence names for the default display,
// and the buffer names when ANGLE has what the images are made with
// (EGL_KHR_image_base, EGL_ANGLE_image_d3d11_texture, EGL_EXT_device_query).
// Everything else is ANGLE's.
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
// (EGL_SYNC_REUSABLE_KHR) are passed to it. A fence made in a context waits
// for that context's commands (glFinish) when it is made and is signaled from
// then on: it never signals before the GPU work before it is done.

struct Sync {
  EGLenum type = 0;
  EGLDisplay display = EGL_NO_DISPLAY;
  int fd = -1;  // a native fence's, signaled when the fence is
};

std::mutex g_syncs_mutex;
std::unordered_set<Sync*>& Syncs() {
  static auto* set = new std::unordered_set<Sync*>();
  return *set;
}

Sync* FindSync(void* sync) {
  auto* s = static_cast<Sync*>(sync);
  std::lock_guard<std::mutex> lock(g_syncs_mutex);
  return Syncs().count(s) != 0 ? s : nullptr;
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

// The current context's commands, finished (EGL_BAD_MATCH without a context
// current on `dpy`, as for a fence made with no context).
bool FinishCurrentContext(EGLDisplay dpy) {
  const gpu::Gl* g = G();
  if (g->eglGetCurrentContext() == EGL_NO_CONTEXT || g->eglGetCurrentDisplay() != dpy) {
    SetError(EGL_BAD_MATCH);
    return false;
  }
  static auto finish = reinterpret_cast<decltype(&::glFinish)>(gesher_egl_proc("glFinish"));
  finish();
  return true;
}

EGLSyncKHR CreateFence(EGLDisplay dpy, EGLenum type, const std::vector<EGLint>& attribs) {
  int fd = EGL_NO_NATIVE_FENCE_FD_ANDROID;
  for (size_t i = 0; i + 1 < attribs.size() && attribs[i] != EGL_NONE; i += 2) {
    if (type == EGL_SYNC_NATIVE_FENCE_ANDROID && attribs[i] == EGL_SYNC_NATIVE_FENCE_FD_ANDROID) {
      fd = attribs[i + 1];
    } else {
      SetError(EGL_BAD_ATTRIBUTE);
      return EGL_NO_SYNC_KHR;
    }
  }
  if (dpy != DefaultDisplay()) {
    SetError(EGL_BAD_DISPLAY);
    return EGL_NO_SYNC_KHR;
  }
  if (fd < 0) {
    if (!FinishCurrentContext(dpy)) return EGL_NO_SYNC_KHR;
    if (type == EGL_SYNC_NATIVE_FENCE_ANDROID) {
      fd = SignaledFenceFd();
      if (fd < 0) {
        SetError(EGL_BAD_ALLOC);
        return EGL_NO_SYNC_KHR;
      }
    }
  }
  auto* s = new Sync();
  s->type = type;
  s->display = dpy;
  s->fd = fd;  // given in the attributes: the implementation owns it from now on
  std::lock_guard<std::mutex> lock(g_syncs_mutex);
  Syncs().insert(s);
  return s;
}

bool OurSyncType(EGLenum type) { return type == EGL_SYNC_NATIVE_FENCE_ANDROID || type == EGL_SYNC_FENCE_KHR; }

EGLSyncKHR BIONIC_SYSV CreateSyncKhr(EGLDisplay dpy, EGLenum type, const EGLint* attribs) {
  if (OurSyncType(type)) return CreateFence(dpy, type, IntAttribs(attribs));
  return G()->eglCreateSyncKHR(dpy, type, attribs);
}

EGLSync BIONIC_SYSV CreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib* attribs) {
  if (OurSyncType(type)) return CreateFence(dpy, type, IntAttribs(attribs));
  auto angle = reinterpret_cast<decltype(&::eglCreateSync)>(gesher_egl_proc("eglCreateSync"));
  if (angle == nullptr) return G()->eglCreateSyncKHR(dpy, type, IntAttribs(attribs).data());
  return angle(dpy, type, attribs);
}

EGLBoolean DestroySyncImpl(EGLDisplay dpy, void* sync) {
  Sync* s = FindSync(sync);
  if (s == nullptr) return G()->eglDestroySyncKHR(dpy, sync);
  {
    std::lock_guard<std::mutex> lock(g_syncs_mutex);
    Syncs().erase(s);
  }
  if (s->fd >= 0) close(s->fd);
  delete s;
  return EGL_TRUE;
}

EGLBoolean BIONIC_SYSV DestroySyncKhr(EGLDisplay dpy, EGLSyncKHR sync) { return DestroySyncImpl(dpy, sync); }
EGLBoolean BIONIC_SYSV DestroySync(EGLDisplay dpy, EGLSync sync) { return DestroySyncImpl(dpy, sync); }

bool Signaled(Sync* s, uint64_t timeout) { return s->fd < 0 || PollFence(s->fd, timeout); }

EGLint ClientWaitImpl(EGLDisplay dpy, void* sync, EGLint flags, uint64_t timeout) {
  Sync* s = FindSync(sync);
  if (s == nullptr) return G()->eglClientWaitSyncKHR(dpy, sync, flags, timeout);
  return Signaled(s, timeout) ? EGL_CONDITION_SATISFIED_KHR : EGL_TIMEOUT_EXPIRED_KHR;
}

EGLint BIONIC_SYSV ClientWaitSyncKhr(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout) {
  return ClientWaitImpl(dpy, sync, flags, timeout);
}

EGLint BIONIC_SYSV ClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout) {
  return ClientWaitImpl(dpy, sync, flags, timeout);
}

EGLint WaitImpl(EGLDisplay dpy, void* sync, EGLint flags) {
  Sync* s = FindSync(sync);
  if (s == nullptr) return G()->eglWaitSyncKHR(dpy, sync, flags);
  if (flags != 0) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  // The commands after the wait are issued once the fence has signaled.
  Signaled(s, EGL_FOREVER_KHR);
  return EGL_TRUE;
}

EGLint BIONIC_SYSV WaitSyncKhr(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags) { return WaitImpl(dpy, sync, flags); }

EGLBoolean BIONIC_SYSV WaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags) {
  return WaitImpl(dpy, sync, flags) == EGL_TRUE ? EGL_TRUE : EGL_FALSE;
}

bool SyncAttrib(Sync* s, EGLint attribute, EGLint* value) {
  switch (attribute) {
    case EGL_SYNC_TYPE_KHR:
      *value = static_cast<EGLint>(s->type);
      return true;
    case EGL_SYNC_CONDITION_KHR:
      *value = s->type == EGL_SYNC_NATIVE_FENCE_ANDROID ? EGL_SYNC_NATIVE_FENCE_SIGNALED_ANDROID
                                                         : EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR;
      return true;
    case EGL_SYNC_STATUS_KHR:
      *value = Signaled(s, 0) ? EGL_SIGNALED_KHR : EGL_UNSIGNALED_KHR;
      return true;
    default:
      SetError(EGL_BAD_ATTRIBUTE);
      return false;
  }
}

EGLBoolean BIONIC_SYSV GetSyncAttribKhr(EGLDisplay dpy, EGLSyncKHR sync, EGLint attribute, EGLint* value) {
  Sync* s = FindSync(sync);
  if (s == nullptr) return G()->eglGetSyncAttribKHR(dpy, sync, attribute, value);
  if (value == nullptr) {
    SetError(EGL_BAD_PARAMETER);
    return EGL_FALSE;
  }
  return SyncAttrib(s, attribute, value) ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean BIONIC_SYSV GetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib* value) {
  Sync* s = FindSync(sync);
  if (s == nullptr) {
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
  if (!SyncAttrib(s, attribute, &v)) return EGL_FALSE;
  *value = v;
  return EGL_TRUE;
}

EGLBoolean BIONIC_SYSV SignalSyncKhr(EGLDisplay dpy, EGLSyncKHR sync, EGLenum mode) {
  if (FindSync(sync) != nullptr) {
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
  Sync* s = FindSync(sync);
  if (s == nullptr || s->fd < 0) {  // not a native fence sync
    SetError(EGL_BAD_PARAMETER);
    return EGL_NO_NATIVE_FENCE_FD_ANDROID;
  }
  int fd = dup(s->fd);
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
