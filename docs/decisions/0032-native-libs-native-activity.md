# 0032 — NativeActivity, AInputQueue and AChoreographer for an app's ELF libraries

**Status:** accepted, 2026-10-24 (native-libraries track; verified in ci run 40056663098). The coordinator may renumber.

## Context
NDK games and engines often have no Java code of their own: their Activity is
the framework's `android.app.NativeActivity` (directly, or through the NDK's
`android_native_app_glue`), which loads the app's library and drives it
through `ANativeActivity_onCreate` and the `ANativeActivityCallbacks` the
library fills in. On Android the natives are libandroid_runtime's
`android_app_NativeActivity.cpp` and `android_view_InputQueue.cpp`, libandroid's
`input.cpp` (`AInputQueue_*`, `AInputEvent_*`), and libnativedisplay's
`AChoreographer` (vsync from SurfaceFlinger through a `DisplayEventReceiver`).

What Gesher has: an Activity's window is a Win32 window of the app's own
process with a `Win32Surface` behind ViewRootImpl's `Surface`
([0029](0029-graphics-app-windows.md)); its input arrives on the window's
`InputChannel` ([0027](0027-input-channel-and-win32-translation.md));
`ANativeWindow_*` and EGL/GLES over ANGLE work on such a Surface
([0023](0023-native-libs-ndk-graphics.md)); and loaded code is System V, its
`JNIEnv`/`JavaVM` the JNI bridge's proxies ([0017](0017-native-libs-jni-bridge.md)).

## Options considered
1. **Build AOSP's sources as they are** (with the System V table rewrite the
   input natives use). The callbacks the library fills in would be called with
   the Microsoft ABI, `env`/`vm` would be the runtime's Microsoft-ABI tables,
   and `OpenNativeLibrary` would need libnativeloader's namespace code;
   `InputQueue::attachLooper` takes an `ALooper_callbackFunc`, so the .so's
   System V callback would reach libutils' Looper as a Microsoft-ABI pointer.
2. **Gesher's own copies of the four sources** (`compat/bionic/native_activity.cc`),
   kept close to AOSP's code and structure, with the ABI boundary made explicit.
3. **A Java NativeActivity of Gesher's** replacing the framework's class.
   Apps name `android.app.NativeActivity` (or subclass it); the framework's
   class works once its natives exist.

## Decision
Option 2.

- **Loading**: `loadNativeCode` dlopen()s the library (gesher-base hands ELF
  files to the loader through the JNI bridge, which finds `DT_NEEDED`
  libraries next to it) and looks the entry point up by name (`dlsym` gives
  the raw System V address of anything that is not `JNI_OnLoad`/`Java_*`):
  `ANativeActivity_onCreate`, or the manifest's `android.app.func_name`.
- **The ANativeActivity**: as AOSP's (`NativeCode` derives from it), but `vm`
  and `env` are the bridge's System V proxies (`ProxyJavaVm`, `ProxyJniEnv` in
  `jni_bridge.h`; `env` is the main thread's), `clazz` a global reference,
  `assetManager` the Java AssetManager's native one (what
  `AAssetManager_fromJava` gives), the data and OBB paths and `sdkVersion`
  from the Java side.
- **Calling loaded code**: the entry point and every callback are called
  through pointers of their System V type (`CallSysv`), after
  `EnterLoadedCode()` (the thread's bionic TLS block, ADR 0014); Java calls
  them on the main thread.
- **The window** is ViewRootImpl's Surface, which NativeActivity takes with
  `Window.takeSurface` (ViewRootImpl then draws nothing itself and has no
  hardware renderer): `ANativeWindow_fromSurface` gives its `Win32Surface`,
  held while it is the activity's window, so the library's `ANativeWindow_lock`
  and `eglCreateWindowSurface` work on it as tested in 0023. Nothing in the
  window session is specific to NativeActivity.
- **Requests from the library** (`ANativeActivity_finish`, `setWindowFormat`,
  `setWindowFlags`, `showSoftInput`, `hideSoftInput`) are written to a pipe the
  main thread's Looper watches, which calls the Activity's methods, as AOSP's.
- **Input**: `android.view.InputQueue`'s natives and libandroid's
  `AInputQueue_*` are one class over libinput's `KeyEvent`/`MotionEvent`
  (`PooledInputEventFactory`), as AOSP's `InputQueue`: ViewRootImpl's native
  input stages send each event (keys first pre-dispatched); the queue's read fd
  is readable while events wait (one byte written when it stops being empty,
  read when it empties, so no read blocks); `AInputQueue_attachLooper`'s
  callback is wrapped in a `LooperCallback` that calls it as System V;
  finished events go back to the Java `InputQueue` on the main thread's Looper.
  `AInputEvent_*`, `AKeyEvent_*`, `AMotionEvent_*` (with `fromJava`/`toJava`)
  read libinput's events as `native/android/input.cpp` does.
- **AChoreographer** (`compat/bionic/choreographer.cc`): no compositor, so the
  vsync is a 60 Hz clock on `CLOCK_MONOTONIC` (multiples of 16,666,667 ns).
  A thread's choreographer is a `MessageHandler` on its Looper (the glue's
  `ALooper_prepare`d thread, or Java's main Looper), woken with
  `sendMessageAtTime` at the next vsync that has callbacks due; it runs them
  there, so a library that polls its Looper (`ALooper_pollOnce`) gets them as on
  Android. `postFrameCallback(64)`, the delayed forms, `postVsyncCallback` (one
  frame timeline: presentation and deadline one period later) and the refresh
  rate callback (told the 60 Hz period once).
- **Registration**: `register_gesher_native_activity` is one row of
  libandroid_runtime's table (compat/framework/android_runtime.cc); the
  libandroid entries go into the bionic shim's table from static initializers.

## Consequences
- An APK with `android:hasCode="false"` whose Activity is
  `android.app.NativeActivity` runs, in a single process and on the device;
  NativeActivity subclasses in an app's dex work the same way.
- The display is a fixed 60 Hz; frame times are not real presentation times.
  The IME (`showSoftInput`) is whatever the window's InputMethodManager does
  (no soft keyboard on Windows).
- NativeActivity asks for an RGB_565 window; Gesher's `Win32Surface` has 8-bit
  formats only, so a library that locks the window without
  `ANativeWindow_setBuffersGeometry` gets RGBA_8888 (most set the format;
  EGL's `EGL_NATIVE_VISUAL_ID` is RGBA_8888).
- `onConfigurationChanged`, `onLowMemory` and `onSaveInstanceState` are
  dispatched as on Android but not exercised in CI.
- The window's input is the window session's (WindowSession.attachInput,
  the graphics track's), and NativeActivity asks for its OBB and external
  files directories, so the services' `mount` stand-in (no volumes) is
  needed: without it Context.getObbDir throws.
