# 0017. Graphics: the Win32 window and Win32Surface

Status: accepted (graphics track, Phase 3). The coordinator may renumber.

## Context

hwui and Android's `android.view.Surface` draw into an `ANativeWindow`. On a
device that is libgui's `Surface`, a BufferQueue producer whose buffers
SurfaceFlinger composes. Gesher has no compositor and no gralloc: each Android
window becomes a top-level Win32 window, drawn by the CPU (Skia raster) until
ANGLE runs. CI runners must verify what was drawn without relying on a
visible desktop.

## Decision

- **`gesher::Window`** (compat/hwui/window/gesher_window.{h,cc}): one thread
  per HWND creates the window and runs its message loop, so the window stays
  responsive whatever the app's threads do. Its back buffer is system memory,
  32-bit BGRA top-down (GDI's order); `WM_PAINT` and `WM_PRINTCLIENT` copy it
  with `SetDIBitsToDevice`. `PrintWindow` therefore reads back what was drawn
  off-screen (it repaints into a memory DC), and tests read pixels that way
  or from the back buffer. Input messages become `WindowEvent`s in a queue
  with a gesher-base eventfd (a Looper can watch it); `WM_CLOSE` is only an
  event.
- **`gesher::Win32Surface`** (win32_surface.{h,cc}) is hostgraphics'
  `android::Surface`, the host form of libgui's, so core/jni's host Surface
  natives, hwui and the NDK functions take it unchanged. It owns its window
  and one buffer in Android's byte order (`PIXEL_FORMAT_RGBA_8888` by
  default), handed out by `lock()` and by `dequeueBuffer()` (`reserved[0]`,
  what hwui's CPU pipeline draws into); `unlockAndPost()`/`queueBuffer()`
  present it, converting RGBA to BGRA under the window's lock. One persistent
  buffer keeps the last frame, so dirty-rectangle redraws work.
- **Skia's `kN32` is RGBA** (`SK_R32_SHIFT 0` in the generated
  SkUserConfig.h), as on Android, instead of Skia's Windows default (BGRA):
  hwui maps `Bitmap.Config.ARGB_8888` and `PIXEL_FORMAT_RGBA_8888` to kN32,
  and apps read bitmap bytes as RGBA. The single conversion is at present.
- **NDK entry points** (native_window.cc, gesher_native_window.h):
  `ANativeWindow_lock`, `unlockAndPost`, `setBuffersGeometry`,
  `setBuffersTransform`, `set/getBuffersDataSpace`, `setFrameRate*`, and
  `ANativeWindow_toSurface` (window_jni.cc), implemented as libnativewindow
  does (`perform()` on whatever window they get); hostgraphics has the rest.
  They are Microsoft-ABI C functions in dalvikvm; libandroid (native-libs
  track) binds Android .so files to them through its System V thunks.
  `gesher_window_create/show/hwnd` make a window with its ANativeWindow.
- **Java**: `com.gesher.app.window.NativeWindow` (gesher-app) creates a window
  and hands out `android.view.Surface`s over its Win32Surface; its natives
  register from hwui's table when the class is on the class path.
- **Device-only natives** (SurfaceControl, SurfaceSession, BLASTBufferQueue)
  answer zero/null or a fresh handle (compat/framework/device_standins.cc);
  `BLASTBufferQueue.nativeGetSurface` asks a provider the window code installs
  (`gesher_set_blast_surface_provider`).

## Consequences

- The CPU pipeline (SkiaCpuPipeline) draws into the Surface's buffer in place
  and its `swapBuffers` does nothing: a frame shows when something presents
  it (`NativeWindow.present()` today). The window path for real apps needs a
  present after each frame: a small hwui patch (queueBuffer in
  `SkiaCpuPipeline::swapBuffers`) or a frame-complete callback.
- One buffer per surface means no tearing protection beyond the window lock;
  acceptable for CPU drawing, replaced when ANGLE presents with a swap chain.
- A window resize changes the buffer size on the next lock/dequeue; hwui's
  CPU pipeline keeps the old buffer until its surface is set again.
