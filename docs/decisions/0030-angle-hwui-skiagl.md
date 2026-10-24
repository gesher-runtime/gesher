# 0030 — hwui's SkiaGL pipeline over ANGLE

**Status:** accepted, 2026-10-24 (graphics track, the GL half of ANGLE)

## Context
hwui renders a RenderNode tree with one of its pipelines. Gesher builds hwui's
host variant (layoutlib's), which has only the CPU one (SkiaCpu,
`HWUI_NULL_GPU`): Skia raster into the Win32 window's back buffer
([0021](0021-graphics-win32-window.md)). On a device hwui renders with
SkiaGL (Skia's Ganesh backend on OpenGL ES through EGL) or SkiaVulkan.
Gesher has ANGLE's EGL 1.5 / OpenGL ES 3.0 on Direct3D 11
([0018](0018-angle-build.md)), and the native-libs track gives an app's .so
Android's EGL meanings on it, window surfaces on a Gesher window's HWND
included ([0023](0023-native-libs-ndk-graphics.md)).

What the device's GL pipeline needs that the host build lacks:
- Skia's Ganesh and its GL backend: in Skia's Android.bp only in libskia's
  `android` variant; Skia's GL interface factories are per platform (EGL,
  WGL, GLX) and link the GL library.
- hwui's GPU half: `SkiaOpenGLPipeline`, `SkiaGpuPipeline`, `EglManager`,
  `GLUtils` (the `android` target), the device's pipeline headers
  (`platform/android/pipeline/skia`; the host has stubs under
  `platform/host`), and a RenderThread with an EglManager and a
  GrDirectContext (the device's `renderthread/RenderThread.cpp` also needs
  AChoreographer, Vulkan, the shader cache, hardware bitmaps).
- The EGL and GLES functions hwui calls itself, with Android's meanings
  (`EGL_DEFAULT_DISPLAY`, an `ANativeWindow*` as the native window,
  `eglBeginFrame`, `EGL_KHR_swap_buffers_with_damage` required).
- A way to choose the pipeline that cannot crash: `EglManager` aborts the
  process when EGL fails.

## Options considered
1. **A Gesher pipeline class** (an `IRenderPipeline` of our own with its EGL
   and GrDirectContext). Free of hwui's device assumptions, but a second GL
   pipeline to keep in step with hwui's, and what Android apps get on a
   device (partial updates, swap behavior, color modes, layers) would be
   reimplemented.
2. **hwui's device GL pipeline in the host build**, its sources as AOSP has
   them, with Gesher supplying the RenderThread's GL half and the EGL/GLES
   entry points over ANGLE. No AOSP patch.
3. **hwui's device variant entirely** (the device RenderThread, CacheManager,
   ShaderCache, ...). Pulls in AChoreographer, Vulkan, AHardwareBuffer and
   libgui; far more than the GL path needs.

## Decision
Option 2.
- **Skia**: Ganesh's sources come from libskia's `android` variant
  (`gesher_bp_cc ... TARGETS android`, filtered to `src/gpu`, `src/text/gpu`
  and `src/sksl/codegen`, less Vulkan, AHardwareBuffer and `gl/egl`);
  `SK_GANESH` and `SK_GL` are added to the generated SkUserConfig.h, as in
  Android's. `GrGLMakeNativeInterface`
  (`compat/hwui/gl/GrGLMakeNativeInterface_angle.cc`) assembles the GLES
  interface from `libGLESv2.dll` next to the executable, else ANGLE's
  `eglGetProcAddress`; null without ANGLE.
- **hwui**: the device's `SkiaOpenGLPipeline.cpp`, `SkiaGpuPipeline.cpp`,
  `EglManager.cpp`, `GLUtils.cpp`, `SkiaProfileRenderer.cpp` are compiled
  into the host hwui, with the device's `SkiaGpuPipeline.h` and
  `SkiaOpenGLPipeline.h` copied ahead of `platform/host`'s.
  `compat/hwui/gl/RenderThread.cpp` is the host's RenderThread plus the
  device's GL context code (an `EglManager`; `requireGlContext` makes a
  GrDirectContext over `GrGLMakeNativeInterface`; `setGrContext`,
  `destroyRenderingContext`), without the CacheManager's device GPU budget.
  The Vulkan pipeline's name stands for the GL one (CanvasContext names it;
  it is never chosen). Stand-ins for what is linked but not reached:
  `DeferredLayerUpdater` (TextureView layers; its constructor aborts),
  `SkSurfaces::WrapAndroidHardwareBuffer`, `AHardwareBuffer_acquire/
  release`, libsync's `sync_wait`, libgraphicsenv's `GpuStatsInfo` header.
- **EGL/GLES for hwui**: hwui is compiled with Android's EGL native types
  (`compat/hwui/gl/include/EGL/eglplatform.h`: `EGLNativeWindowType` is
  `ANativeWindow*`, no `<windows.h>`) and without dllimport.
  `compat/hwui/gl/angle_gl.cc` defines the ~45 functions hwui calls, each
  resolving ANGLE's on first use. Display, configs and window surfaces go
  through `compat/bionic/egl_gles.cc`, which gained Microsoft-ABI entry
  points for code in the executable (`gesher/egl_angle.h`): the same
  default display (Direct3D 11 hardware, else WARP;
  `GESHER_EGL_DEVICE=warp`), the same window surfaces (ANGLE's swap chain on
  the Gesher window's HWND, the window's `WM_PAINT` switched off while it
  exists, a reference on the ANativeWindow), the same Android config
  attributes. One implementation for an app's .so and for hwui.
  `eglBeginFrame` does nothing; `EGL_KHR_swap_buffers_with_damage` is
  reported and served by `eglSwapBuffers` when ANGLE's display lacks it;
  `eglDupNativeFenceFDANDROID` gives no fence (ANGLE does not report
  `EGL_ANDROID_native_fence_sync`).
- **Choosing the pipeline** (`compat/hwui/gl/pipeline_select.cc`, called
  where `register_gesher_graphics` used to force SkiaCpu): the CPU pipeline
  stays the default. `GESHER_HWUI_RENDERER=skiagl`, else the system property
  `debug.hwui.renderer=skiagl` (Android's switch), asks for SkiaGL; it is
  used only if a check passes first (ANGLE loads, its display initializes,
  an RGBA8 ES context on a pbuffer becomes current, Skia gets a valid
  GrGLInterface). Otherwise one line on stderr says why and SkiaCpu renders.
  hwui fixes the pipeline for the process before anything draws.
- **Reading GL frames back** (`com.gesher.app.graphics.GraphicsDebug`,
  gesher-app; `compat/hwui/gl/graphics_debug_jni.cc`): the chosen pipeline;
  `captureComposed(hwnd)`, PrintWindow with `PW_RENDERFULLCONTENT` (what DWM
  composed, swap chain included; `NativeWindow.capture` uses `PW_CLIENTONLY`
  alone, which sees the CPU back buffer); and a debug hook
  (`setGlReadback`, or `GESHER_HWUI_GL_READBACK=1`) that reads each window
  frame with `glReadPixels` just before `eglSwapBuffers`, for
  `lastGlFrame(hwnd)`.

## Consequences
- The GL path is AOSP's code; following hwui means rebuilding, not porting.
  Skia grows by Ganesh (a few hundred files) for every Skia user.
- The CPU pipeline and its back buffer path are untouched when GL is off;
  with GL on, the window's back buffer is not updated (the swap chain
  presents) and `NativeWindow.capture`/`readBackBuffer` show the last CPU
  frame, if any.
- Window surfaces need a Gesher window (Win32Surface); a resize is picked up
  by ANGLE's swap chain at the next swap.
- Not available on GL: TextureView layers (DeferredLayerUpdater), hardware
  bitmaps, HardwareBufferRenderer, WebView functors, the persistent shader
  cache and the device's GPU memory budget. Vulkan is not built.
- What CI verifies runs on WARP (the runners have no GPU; their "hardware"
  adapter is the Microsoft Basic Render Driver); a real adapter is ANGLE's
  default and untested. ci run 40047748570 (both runners): the SkiaGL frame
  of a RenderNode tree with a child node, text and a bitmap equals the CPU
  pipeline's (glReadPixels and PrintWindow with PW_RENDERFULLCONTENT), chosen
  by the environment variable and by the system property; without ANGLE the
  CPU pipeline renders.
