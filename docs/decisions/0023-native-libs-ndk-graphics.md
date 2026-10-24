# 0023 — The NDK's graphics for an app's ELF libraries: ANativeWindow and EGL/GLES over ANGLE

**Status:** accepted, 2026-10-23 (native-libraries track)

## Context
NDK code draws in two ways: into an `ANativeWindow`'s CPU buffer
(`ANativeWindow_lock`/`unlockAndPost`), or with OpenGL ES through EGL
(`eglCreateWindowSurface` on the `ANativeWindow`, `eglSwapBuffers`). Its
imports name `libandroid.so`, `libEGL.so`, `libGLESv1_CM.so`, `libGLESv2.so`
or `libGLESv3.so`, and it calls them with the System V ABI.

What Gesher has: the graphics track's Win32 window and `Win32Surface`, a
hostgraphics `Surface` over it ([0021](0021-graphics-win32-window.md)), with
the NDK's `ANativeWindow_*` functions compiled into dalvikvm with the
Microsoft ABI (`compat/hwui/window/gesher_native_window.h`); and ANGLE's
`libEGL.dll`/`libGLESv2.dll` (EGL 1.5, OpenGL ES 3.0 on Direct3D 11,
[0018](0018-angle-build.md)), installed next to `dalvikvm.exe`, Microsoft
ABI too. The bionic shim satisfies an app's platform libraries symbol by
symbol ([0020](0020-native-libs-libandroid.md)).

## Options considered
1. **Load ANGLE's DLLs as the `.so`'s libraries** and let the ELF loader bind
   their exports. Impossible as it is: the exports are Microsoft ABI, the
   `.so` calls System V.
2. **Hand-written System V wrappers** for the functions a test app uses.
   EGL/GLES are about 1,200 functions with extensions; apps reach any of them,
   also through `eglGetProcAddress`. A partial list fails at run time.
3. **Wrappers generated from the Khronos headers**, each a `sysv_abi`
   function with the header's prototype that calls ANGLE's entry point
   through a pointer of the Microsoft-ABI type, so the compiler converts the
   arguments (integer and vector registers are assigned differently, the
   stack layout and the callee-saved `xmm6-15` differ). Hand-written only
   where Android's EGL and ANGLE's differ in meaning.
4. **Link dalvikvm against ANGLE's import libraries.** Would make dalvikvm
   fail to start without the DLLs; loading them on first use keeps EGL
   optional.

## Decision
- **Option 3, with lazy loading (not 4).** `compat/bionic/gen_gl_thunks.py`
  reads AOSP's `frameworks/native/opengl/include` (the headers the NDK ships:
  EGL 1.5 and its extensions, GLES 1.x, 2.0, 3.0-3.2 and their extensions) at
  build time and writes a thunk per function; `egl_gles.cc` adds them to the
  shim's table. The first call of each resolves ANGLE's export
  (`GetProcAddress` on `libEGL.dll`/`libGLESv2.dll`, loaded from the
  executable's directory, else `eglGetProcAddress`); a function ANGLE lacks
  aborts naming it. A header function that takes or returns a callback fails
  the generator unless it is handwritten or explicitly left out, so new ones
  are not forwarded wrongly.
- **Android's EGL meanings, handwritten:**
  - `EGL_DEFAULT_DISPLAY` (and `EGL_PLATFORM_ANDROID_KHR`) is ANGLE's
    Direct3D 11 display on the hardware adapter, initialized at once to see
    that it works, else on WARP; `GESHER_EGL_DEVICE=warp` asks for WARP.
  - The native window of `eglCreateWindowSurface` and
    `eglCreatePlatformWindowSurface(EXT)` is an `ANativeWindow*`; a Gesher
    window's HWND (`gesher_window_hwnd`) gets ANGLE's window surface, and
    the surface holds a reference to the `ANativeWindow` until
    `eglDestroySurface`, as Android's EGL connects to the window.
  - Configs: `EGL_RECORDABLE_ANDROID`/`EGL_FRAMEBUFFER_TARGET_ANDROID` are
    dropped from `eglChooseConfig`'s attributes (ANGLE rejects them) and
    answered by `eglGetConfigAttrib`; `EGL_NATIVE_VISUAL_ID` is an Android
    HAL pixel format, which NDK code passes to
    `ANativeWindow_setBuffersGeometry`.
  - `eglGetProcAddress` returns the System V thunk; callbacks the `.so`
    passes (`glDebugMessageCallback(KHR)`, `eglDebugMessageControlKHR`,
    `eglSetBlobCacheFuncsANDROID`) reach ANGLE as Microsoft-ABI trampolines.
- **Who paints a window that GL draws to.** The Gesher window paints its CPU
  back buffer on `WM_PAINT`/`WM_PRINTCLIENT` (gesher_window.cc). While an EGL
  window surface exists on it, the swap chain presents instead: `egl_gles.cc`
  subclasses the HWND (`GWLP_WNDPROC`, allowed across threads of one
  process) so `WM_PAINT` only validates and `WM_ERASEBKGND` does nothing,
  and restores the window procedure with the last surface. No change to the
  graphics track's files was needed; a hook in `gesher_native_window.h`
  (e.g. "the window is presented by someone else") would replace the
  subclassing if the window code ever needs to know.
- **ANativeWindow functions** are System V thunks over dalvikvm's
  Microsoft-ABI ones (`libandroid_window.cc`); `ANativeWindow_fromSurface`/
  `toSurface` pass the runtime's real `JNIEnv` (`RealJniEnv`).

## Consequences
- Every EGL/GLES entry point an NDK library can import or look up exists;
  what ANGLE implements works (ES 3.0 on the CI runners' WARP; 3.1 on capable
  hardware is untested), the rest fails visibly when called.
- dalvikvm starts without ANGLE; `eglGetDisplay` gives `EGL_NO_DISPLAY` with
  a message when the DLLs are missing.
- Window surfaces need a Gesher window (a `Win32Surface`); other
  `ANativeWindow`s (an `ImageReader`'s, a `SurfaceTexture`'s) get
  `EGL_BAD_NATIVE_WINDOW`. Pixmap surfaces are passed through and fail.
- When the graphics track's Activity windows (IWindowSession) arrive, their
  Surfaces are `Win32Surface`s, so `ANativeActivity`'s
  `onNativeWindowCreated` window works with this unchanged.
