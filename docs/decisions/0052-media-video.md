# 0052: Video: Media Foundation decoders, MediaCodec, surfaces drawn by hwui

Status: accepted (media track, stage 2; docs/media.md "Video")

## Context
Apps play video through MediaCodec (with MediaExtractor, or ExoPlayer's own
extractors), MediaPlayer/VideoView and MediaMetadataRetriever, and show it in a
SurfaceView or a TextureView (SurfaceTexture). On Android the frames reach the
screen without the app's window being redrawn: SurfaceFlinger composes the
video layer under (or over) the window, and TextureView's layer is a GL
texture hwui samples. Gesher has neither SurfaceFlinger nor GL textures for
the CPU pipeline; each Android window is one Win32 window that hwui draws.

## Decision
- **Decoders**: Media Foundation's synchronous decoder MFTs where Windows has
  one for the codec (H.264 everywhere Media Foundation exists; H.265, VP8, VP9,
  AV1, MPEG-4 part 2 and H.263 where the edition or the Store's codec
  extensions provide them), in system memory with NV12 output converted to
  I420; AOSP's software decoders (libvpx, libdav1d, libhevc) for the codecs a
  machine's Media Foundation lacks. Media Foundation is loaded at run time:
  editions without it keep the software decoders. Hardware decoding (DXVA,
  a D3D11 device manager) is a later step: the MFTs run in software here.
- **MediaCodec** is Gesher's own over these decoders (not libstagefright's
  ACodec/CCodec): fixed input and output slots in native memory as direct
  ByteBuffers, a worker thread, synchronous dequeue calls and the asynchronous
  callback model (postEventFromNative), video as I420
  (COLOR_FormatYUV420Planar; also as a YUV_420_888 Image), audio through the
  player's audio decoders. With an output Surface, a released buffer is drawn
  into the Surface's ANativeWindow (RGBA, BT.601). MediaCodecList lists the
  decoders this machine really has, with capabilities in the legacy form
  CodecCapsLegacyImpl takes. No encoders yet.
- **Surfaces without a compositor**: a SurfaceTexture's or a SurfaceView's
  Surface is a QueueSurface, a hostgraphics Surface whose frames stay in the
  process (the latest frame, immutable). hwui draws that frame where the view
  is, as part of the window's own frame:
  - TextureView: HardwareRenderer's texture layer is Gesher's (not hwui's
    DeferredLayerUpdater, which needs the device's ASurfaceTexture);
    RecordingCanvas.nDrawTextureLayer records an SkDrawable that draws the
    latest frame when the display list plays back, a raster image on the CPU
    pipeline and on SkiaGL alike. Each frame posts SurfaceTexture's
    frame-available event, so TextureView invalidates as on Android.
  - SurfaceView: frameworks/base patch 0300 makes SurfaceView draw its
    surface's latest frame into the hole it punches (or in place when it is
    above its window) through a native method, and each frame invalidates the
    view. Views above the SurfaceView (controls, subtitles) are drawn over it,
    as SurfaceFlinger would compose them.
  The cost: every video frame redraws the window (the CPU pipeline renders the
  whole frame); there is no zero-copy path.
- **MediaPlayer** plays a file's video track with its audio: a video thread
  decodes and shows each frame at its time on the media clock (the audio
  track's played position; the wall clock for a video without audio), drops
  frames more than 100 ms late, shows the first frame after a seek at once,
  and reports the video size (MEDIA_SET_VIDEO_SIZE). VideoView works over it.

## Consequences
TextureView and SurfaceView show video in both of hwui's pipelines with the
z-order the app laid out. A SurfaceView used by GL (GLSurfaceView) or with a
canvas from lockCanvas through its own EGL context is not covered: EGL window
surfaces exist only for Win32 windows. Protected content (MediaCrypto/DRM),
tunneled playback and encoders (MediaRecorder, MediaMuxer's inputs) are not
available. HDR and 10-bit output are converted to 8-bit I420.
