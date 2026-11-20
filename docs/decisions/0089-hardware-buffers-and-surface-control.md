# 0089: AHardwareBuffer, ASurfaceControl and EGL native buffers on Windows

Status: accepted for implementation, 2026-11-20. Verified locally with the NDK
test library and its host runner, and end to end with Opera (Verification).

## Context

Chromium (Opera, com.opera.browser) needs three NDK graphics contracts of
Android 10 and later in its GPU process, and Gesher provided none of them:

- **ASurfaceControl / ASurfaceTransaction** (`libandroid.so`, API 29+).
  Chromium's display compositor presents through SurfaceControl on Android 10
  and later (`gl::GLSurfaceEGLSurfaceControl`). Its loader
  (`ui/gfx/android/android_surface_control_compat.cc`) `dlsym`s the whole set
  from `libandroid.so` and treats a missing one as fatal for the device's SDK
  level: `[FATAL:...android_surface_control_compat.cc:240] Unable to load
  function ASurfaceControl_createFromWindow`. At SDK 36 the set includes the
  API 30-34 functions (`setOnCommit`, `setCrop`/`setPosition`/`setScale`,
  `setFrameRate`, `setFrameTimeline`, `setEnableBackPressure`,
  `setExtendedRangeBrightness`, `ASurfaceControl_fromJava`) and the stats
  queries.
- **AHardwareBuffer** (`libnativewindow.so`/`libandroid.so`, API 26+):
  `libopera.so` imports `allocate`, `describe`, `acquire`/`release`,
  `lock`/`lockPlanes`/`unlock` and `send`/`recvHandleFromUnixSocket`; the
  display compositor's frame buffers are AHardwareBuffers
  (`AHardwareBufferImageBacking`).
- **EGL import of hardware buffers and native fences**:
  `eglGetNativeClientBufferANDROID` + `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)`
  (`gpu::CreateEGLImageFromAHardwareBuffer`), and
  `EGL_ANDROID_native_fence_sync` (`eglDupNativeFenceFDANDROID`), which
  Chromium assumes is present whenever the entry point resolves
  (`ui/gl/gl_display.cc`) and uses for every access to such a buffer.

ANGLE's Direct3D 11 backend, Gesher's EGL/GLES (ADR 0023, 0030), has none of
the EGL_ANDROID buffer or fence extensions and no EGL fence syncs
(`EGL_KHR_fence_sync`). Gesher has no compositor process: a SurfaceView's
surface is a QueueSurface whose frames hwui draws (ADR 0056), and Chromium's
GPU process holds a forwarding Surface whose frames travel to the browser's
SurfaceView (`compat/framework/surface_parcel.cc`).

Lowering the reported SDK level or claiming the device lacks SurfaceControl
would change what the application is told about the platform; both are out of
scope (CONTRIBUTING.md). The contracts are implemented instead.

## Decision

### 1. AHardwareBuffer is shared memory with a per-process GPU form

- A buffer is a memfd (stride x height pixels, stride = width) mapped in the
  process, its `AHardwareBuffer_Desc` and an id `pid << 32 | count` (as
  GraphicBuffer's). `AHardwareBuffer_lock`/`lockAndGetInfo`/`lockPlanes`
  return the mapping; `unlock` completes before it returns (fence -1, which the
  NDK defines as "already complete").
- What is served, and what `AHardwareBuffer_isSupported` answers: formats
  R8G8B8A8, R8G8B8X8, R8G8B8, R5G6B5, R16G16B16A16_FLOAT, R10G10B10A2, R8 and
  BLOB; one layer; CPU read/write, GPU sampling, GPU color output and composer
  overlay usage (vendor bits are accepted as the hints they are). Protected
  content, video encoding, sensor data, GPU data buffers, cube maps, mipmaps,
  multi-layer and YUV buffers are refused (`-EINVAL`, `isSupported` 0); GPU
  usage needs ANGLE's device and fits its texture size. Invalid descriptions
  follow AOSP's `AHardwareBuffer_isValidDescription`.
- `AHardwareBuffer_sendHandleToUnixSocket` sends the description and the
  memfd with `SCM_RIGHTS` (ADR 0088's broker between processes); the receiver
  maps the same memory, so CPU writes are shared as on Android.

### 2. A GPU thread per process holds each buffer's texture

A thread per process (`compat/bionic/gpu_interop.cc`) owns a Direct3D 11
device of its own, created on the adapter of ANGLE's device (Android's default
EGL display; `EGL_EXT_device_query`, `EGL_ANGLE_device_d3d`). It creates a
buffer's texture when the buffer is first used by the GPU, as a shared
resource that ANGLE's device opens (`OpenSharedResource`, a free-threaded
device method). Each `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)` makes an
EGLImage of ANGLE's view of that texture (`EGL_ANGLE_image_d3d11_texture`;
RGBX, RGB and RGB565 buffers are RGBA8 textures whose images are RGB), which
holds a reference to the buffer until it is destroyed; the app's textures of
those images and the compositor sample one storage. The thread never makes an
ANGLE context current: ANGLE's Direct3D 11 back end keeps one device context
and state cache per display, which an extra context of Gesher's beside the
app's would corrupt (section 6). Work crosses devices only after it is
complete: the thread waits for its own uploads before ANGLE's device reads
them, and the app's writes are complete when the fences of section 3 signal.
Memory and texture are synchronized at Android's synchronization points:

- a CPU lock reads the texture back when the GPU may have drawn into it (the
  buffer has been imported as an image), within the locked rectangle;
- a CPU unlock after writing uploads the written rectangle to the texture;
- a buffer received from another process is uploaded when a transaction or an
  import takes it (its memory is the other process's writes);
- `sendHandleToUnixSocket` reads the texture back first, so the receiver maps
  what was drawn so far.

GPU writes made in one process after a buffer was sent are not visible in
another process until the writer reads the buffer back (a CPU lock there).
This is a difference from gralloc, where both processes see one buffer; it is
documented here and in docs/native-libs.md.

### 3. EGL: native client buffers, images of them, fence syncs

`compat/bionic/egl_native_buffer.cc` handles these for the default display,
through the same table as the other handwritten EGL entry points (the bionic
shim's symbols and `eglGetProcAddress`), and adds
`EGL_ANDROID_get_native_client_buffer`, `EGL_ANDROID_image_native_buffer`,
`EGL_KHR_fence_sync`, `EGL_KHR_wait_sync` and `EGL_ANDROID_native_fence_sync`
to `eglQueryString(EGL_EXTENSIONS)`, the buffer names when ANGLE has what the
images are made with (`EGL_KHR_image_base`, `EGL_ANGLE_image_d3d11_texture`,
`EGL_EXT_device_query`; the GPU thread starts only when a buffer is used).
Errors are left in ANGLE's per-thread error state, so `eglGetError` reports
them.

- An AHardwareBuffer has GraphicBuffer's layout: its first 16 bytes stand for
  RefBase (a vtable and `mRefs`) and an `ANativeWindowBuffer` follows, filled
  in as GraphicBuffer fills it (magic, size, stride, HAL format, layer count,
  usage, a native handle with the memfd, System V `incRef`/`decRef`).
  `eglGetNativeClientBufferANDROID` returns that `ANativeWindowBuffer`, as on
  Android, and `EGL_NATIVE_BUFFER_ANDROID` images take it. Code in apps relies
  on this: ANGLE, which Chromium bundles and runs on the system's GLES
  (Opera does), computes the client buffer as the buffer + 16 and reads those
  fields before it calls the system's `eglCreateImageKHR`.
- `EGL_KHR_fence_sync` and `EGL_KHR_wait_sync` are needed under the native
  fences: `EGL_ANDROID_native_fence_sync` is written against them, and
  ANGLE's GL back end loads `eglCreateSyncKHR`/`eglWaitSyncKHR` only when the
  system names them (it advertises native fences whenever
  `eglDupNativeFenceFDANDROID` resolves, and called a null `eglCreateSyncKHR`).
  ANGLE's Direct3D 11 display has no EGL fence syncs, so a fence sync
  (`EGL_SYNC_FENCE_KHR`, or `EGL_SYNC_NATIVE_FENCE_ANDROID` without an fd)
  waits for the current context's commands to finish (`glFinish`) when it is
  made and is signaled from then on; a native one holds a signaled fd: a
  `/dev/null` descriptor, which polls readable and crosses processes as a
  handle-backed fd (ADR 0088), but has no `sync_file` ioctls (merging or signal
  times report failure, which Chromium handles as "no information"). So no
  fence signals before the GPU work before it is done. A fence made from an fd
  is waited for by polling it; `eglWaitSyncKHR` waits on the CPU before later
  commands are issued. Reusable syncs stay ANGLE's. The cost is CPU/GPU
  overlap: making a fence blocks until the GPU has caught up.

### 4. ASurfaceControl is a layer tree composed into its window

`compat/bionic/surface_control.cc`:

- `createFromWindow` attaches a layer to an ANativeWindow (any hostgraphics
  Surface: a SurfaceView's or forwarding QueueSurface, a Gesher window);
  `create` makes a child; `reparent` moves a layer, to null off the display;
  visibility hides a subtree; z orders siblings, negative z below the parent's
  own content (SurfaceFlinger's order).
- A layer's content is a buffer (premultiplied, sampled through its texture),
  a color, or both (the color behind the buffer, filling the crop, the buffer,
  or the parent). Its buffer crop, transform (flips, then 90-degree rotation)
  and destination frame come from `setGeometry`, or from `setCrop`,
  `setPosition`, `setScale` and `setBufferTransform`; opaque layers are drawn
  without blending; plane alpha multiplies.
- `ASurfaceTransaction_apply` applies a transaction's changes atomically and
  in order under one lock, snapshots every window whose tree changed (the
  buffers it shows, their acquire fences) and queues the snapshot to the GPU
  thread, which waits for the fences, draws the layers on its device into a
  render target of the window's size and copies it into the window's buffer
  (through a staging texture), as the window's producer would post a frame. Every applied transaction is presented in the
  order applied; none is dropped, so `setEnableBackPressure` holds either way.
  A window whose last layer leaves gets an empty (transparent) frame.
- Callbacks run on a thread of their own, as Android's binder threads:
  OnCommit once the transaction is applied, release callbacks
  (`setBufferWithRelease`, fence -1) and OnComplete after the frame was
  posted. `ASurfaceTransactionStats` give the surfaces the transaction
  touched, the latch time (the app's CLOCK_MONOTONIC when composition latched
  the frame), each new buffer's acquire time (when its fence was seen
  signaled; -1 without a fence), no present fence (-1, which the NDK defines
  for devices without present fences) and previous release fences of -1 (the
  replaced buffer is not read by the frame that was composed without it).
  Querying release or present fences in OnCommit aborts, as on Android.
- Accepted without effect, and documented as such: damage regions (frames are
  composed whole), data spaces and HDR metadata (composed as sRGB on an SDR
  display), frame rates and frame timelines (composition is paced by the
  transactions). `ASurfaceTransaction_setLuts` is not provided.
- `ASurfaceControl_fromJava` gives the layer standing for a Java
  SurfaceControl, one per object. Gesher's Java SurfaceControls are device
  stand-ins that are not composited (docs/graphics.md), so layers under such
  a layer are not shown; it is reported once in the log.

### 5. Input receivers on layers are refused

Chromium's viz also loads the API 35 input transfer API (`AInputTransferToken_*`,
`AInputReceiver_*`, `AInputReceiverCallbacks_*`) at SDK 36 and called a null
pointer without it. `AInputTransferToken_fromJava/toJava/release` wrap the
framework's native InputTransferToken (the input track's
`android_window_InputTransferToken.cpp`); the callbacks objects hold a context
and two callbacks. On Android `AInputReceiver_create*InputReceiver` has the
window manager grant an input channel to the layer (an embedded window) and
move gestures to it; Gesher's window manager has no embedded windows and
routes input by Win32 window, so no receiver can be granted: the functions
return null (logged once). Chromium treats that as a failed creation and keeps
handling input in the browser process, as before.

### 6. GLES contexts current on different threads

Chromium's GPU process keeps two GLES contexts current on two threads for
good: the GPU main thread's (raster) and the display compositor's (DrDc),
each in a share group of its own, exchanging AHardwareBuffers. Android's EGL
allows that, and Gesher's hwui RenderThread draws on the same ANGLE display
as every app thread's context (`compat/hwui/gl/angle_gl.cc`). ANGLE's
Direct3D 11 back end has one device context and one state cache per display; with its default options a context's GL calls
take only that context's mutex (contexts of different share groups run
concurrently), and the cache is reset only by `eglMakeCurrent`. A context
that draws after another context drew, with no state change of its own in
between, then draws with the other context's framebuffer and program
(`tests/native/surface_control_test.c` draws that way from two threads: with
ANGLE's default options the first context's draw went to the second one's
framebuffer).

`compat/angle/args.gn` sets `angle_force_context_check_every_call = true`,
ANGLE's option for this: every GL call takes the global EGL mutex, and a
context that is not the one that made the previous call marks all of its
state dirty before the call, so its own state is applied before it draws.
The cost is the global lock per call (contexts on different threads no
longer run GL calls concurrently, which the single device context could not
do safely anyway) and a full state resync when the calling context changes.

## Consequences

- Chromium's GPU process loads its SurfaceControl set and presents through it:
  each frame is drawn by the GPU into an AHardwareBuffer, composed by the GPU
  thread, read back once (a staging copy into the window's buffer) and
  forwarded to the browser's SurfaceView. That is one GPU read-back and the
  existing forwarding copies per frame, the same order of cost as the EGL
  child surfaces of ADR 0056.
- Native fences cost CPU/GPU overlap (section 3).
- GL calls of all contexts in a process are serialized, and switching between
  contexts resyncs their whole state (section 6).
- The window's own buffers and its layers are not composed together: frames
  the app posts to the same ANativeWindow directly and the layer tree's frames
  replace each other. Applications use one or the other.
- Cross-process GPU writes after a buffer was sent need a CPU lock in the
  writing process to become visible (section 2).

## Rejected

- **Reporting SDK < 29 or no SurfaceControl to Chromium**: a different device
  than the one Gesher presents; forbidden (CONTRIBUTING.md).
- **Native fences returned as -1**: Chromium treats the extension as present
  once its entry point resolves; failing every fence would make its accesses
  unsynchronized and log an error per frame.
- **An ANGLE context on the GPU thread** (textures, images
  `EGL_GL_TEXTURE_2D_KHR` and composition through ANGLE's GLES): the first
  design. ANGLE's Direct3D 11 back end resets its per-display state cache only
  in `eglMakeCurrent`, so the GPU thread's draws and Chromium's, on contexts
  current on different threads, went to each other's render targets
  (Chromium's frames came out black after the first one).
- **Using ANGLE's own device context**: it is ANGLE's and not safe to use
  beside it; only free-threaded device methods are called on ANGLE's device.
- **CPU composition**: the buffers are GPU textures; a GPU composition needs
  one read-back per frame instead of one per buffer.

## Verification

`tests/native/surface_control_test.c` (an NDK library, API 31) and
`tests/native/surface_control_host_test.cc` (its runner with a Gesher window),
run by `tests/windows/tracks/40-native-libs.ps1` on ANGLE's hardware device
with a descriptor broker and on WARP without one.

End to end (locally, by hand): Opera (com.opera.browser) through
`gesher-app.cmd view` loads its SurfaceControl set, composes every frame of
its display compositor into the browser's SurfaceView and shows
https://example.com/ and a Wikipedia article (text, images, scrolling with
the mouse wheel), checked by screenshots of its window. That needs ANGLE
built with section 6's option; with the CI build of the same commit before
it, the page area stayed white (only the page background was composed).
