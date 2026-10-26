# ADR 0056: Child surfaces and textures (surfaces track)

Status: accepted for the verified child-surface stage (surfaces track,
ci 40307919084 attempt 2 at b1b59a6; both runners, package and both e2e jobs).
The remaining contracts below are assigned to surfaces2, ADR 0063 reserved.

## Context

On Android a SurfaceView gets a child layer from SurfaceFlinger (a SurfaceControl and
a BLASTBufferQueue) whose buffers the compositor places behind or above the window; a
TextureView's SurfaceTexture is a BufferQueue consumer hwui samples as a GL texture.
Gesher has no compositor: each window is one Win32 window that hwui draws into
(ADR 0021, 0029). Apps still need these surfaces: games and SDL draw with EGL into a
SurfaceView, video and camera previews go to SurfaceViews and TextureViews, Canvas
draws with lockCanvas. Bitmaps must also cross processes (an ACTION_IMAGE_CAPTURE
thumbnail), which hwui's host build refuses ("Cannot use parcels outside of Linux").

The media track (ADR 0052, stage 2) already gives SurfaceView and SurfaceTexture a
producer surface in the process (QueueSurface, compat/media/surface) whose latest
frame hwui draws where the view is.

## Decision

1. **No compositor; frames drawn by hwui.** Child surfaces are QueueSurfaces (the
   media track's): every frame queued becomes the surface's latest frame and
   invalidates its view; hwui draws it where the view is in the window's next frame,
   on the CPU pipeline and on SkiaGL alike. Position, size, scrolling and clipping
   follow the view tree for free; a SurfaceView below its window (the default)
   is drawn inside the hole it punches, so views drawn after it cover it, as on a
   device.
2. **EGL on surfaces without an HWND.** ANGLE needs a window for a window surface.
   An ANativeWindow that is not a Gesher window gets a hidden helper window of its
   size (its own thread and message loop); before each eglSwapBuffers the default
   framebuffer is read back (the app's pack state and framebuffer bindings saved and
   restored, no GL error raised) and posted to the ANativeWindow with lock /
   unlockAndPost, paced at 60 frames a second. The helper follows the
   ANativeWindow's size (setFixedSize, a resized view); ANGLE resizes its swap chain
   at the next swap (compat/bionic/egl_child_surface.cc).
3. **Bitmaps parcel through android.os.Parcel's Java methods** (in place up to
   12 KiB or when the parcel may not carry descriptors, else an
   android.os.SharedMemory, a memfd that binderd carries as a Windows handle); the
   reader copies into a heap Bitmap (compat/hwui/surfaces/bitmap_parcel.cc).
4. **YuvImage's JPEG encoder** is hwui's YuvToJpegEncoder logic without its
   libultrahdr half (compat/hwui/surfaces/yuv_image.cc); compressToJpegR answers
   false.

## Consequences

- A frame from a SurfaceView costs a hwui frame of the window (the view's dirty
  region on the CPU pipeline). A GL surface costs a glReadPixels per frame.
- What a compositor would do with layers (a child layer's own alpha, corner radius,
  transforms by SurfaceControl transactions an app makes itself) is not modelled
  beyond what the view draws.
- Bitmaps read from a parcel are heap Bitmaps, never shared memory mappings.
- These remaining layer/storage contracts, JPEG_R, external-OES and Java
  EGL/GLES/GLSurfaceView are open in docs/handoff/tracks/surfaces2.md.
