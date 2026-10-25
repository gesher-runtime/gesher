# Track brief: surfaces

Graphics leftovers reported by the hwsvc2 and arm64 tracks. The original
brief given to the first agent of this track (2026-10-26), kept verbatim. A
successor reads it together with the "Status and next steps" section in the
track's doc on its branch.

You are the "surfaces" track agent of the gesher project. Your worktree is
C:\work\gesher-devices (reused) on branch wip/surfaces (cut from
phase4/integration and pushed). Work only there, with absolute paths under
C:\work\gesher-devices (another agent once wrote into the coordinator's
worktree by mistake: check `pwd` before editing).

FIRST read docs/handoff/track-rules.md and follow it exactly: it is binding.
Then read docs/graphics.md (app windows, the hwui/ANGLE path), ADR 0051 and
docs/devices.md (camera: what is done, the SurfaceView gap), docs/arm64.md
(TIC-80), docs/media.md (video output: the media track's stage 2 is in
progress on wip/media and may already render video; read its branch),
compat/hwui/window and how ViewRootImpl's surface gets its native window.

GOAL: Android's child surfaces work, each verified in CI:
- SurfaceView: its SurfaceControl / BLASTBufferQueue child surface gets a
  native window (ANativeWindow) that apps can draw into with EGL/GLES (as
  SDL and games do), with Canvas (`Surface.lockCanvas`), and that
  MediaCodec / MediaPlayer / Camera2 can output to; z-order (behind the
  window with a punched hole by default, `setZOrderOnTop`,
  `setZOrderMediaOverlay`), position and size follow the view (layout,
  scrolling), SurfaceHolder callbacks (surfaceCreated / Changed /
  Destroyed, also on pause/resume and on resize), `setFixedSize`, format.
- TextureView and SurfaceTexture: frames produced into a SurfaceTexture
  (from GLES, Canvas, MediaPlayer, Camera2) reach the TextureView's layer
  in the app's hwui frame; `updateTexImage`, the transform matrix,
  `getBitmap`, `setOnFrameAvailableListener`.
- Camera preview into SurfaceView and TextureView (Camera2 and the legacy
  Camera's setPreviewDisplay/setPreviewTexture), closing hwsvc2's gap.
- TIC-80 (tests/e2e/apps.json, now a knownFailure) reaches its first frame:
  remove `knownFailure` when it passes on both runners.
- Bitmap parcels across processes (hwui's Bitmap_writeToParcel /
  createFromParcel throw "Cannot use parcels outside of Linux": use shared
  memory, ashmem's role, as Android does), so Gesher Camera's thumbnail
  (ACTION_IMAGE_CAPTURE without EXTRA_OUTPUT) reaches the caller; and
  YuvImage's natives (compressToJpeg).
Tests in tests/windows/tracks/52-surfaces.ps1 (test APKs of your own; pixel
read-backs of the window, both runners, single process and on the Gesher
device); e2e must stay green. Your file domain: the hwui/window code for
child surfaces (compat/hwui, shared with windows3: careful, minimal edits;
windows3 is changing the window thread's wait loop and orientation),
SurfaceTexture/TextureView natives, Bitmap parcel and YuvImage natives,
the camera's preview path (CameraService/LegacyCamera, hwsvc2's: minimal
edits), tests/windows/tracks/52-surfaces.ps1 and its test apps
(framework.yml: one line per test app), docs/graphics.md sections, ADR
docs/decisions/0056-surfaces-child-surfaces-and-textures.md (0056 is
reserved for you), the roadmap boxes (also hwsvc2's unticked SurfaceView
box and arm64's TIC-80 box when verified).

Coordinator notes: CI first; NEVER build AOSP locally. Other tracks run in
parallel on the shared CI (media, windows3, aot2, exec2, native2, web3):
trigger CI only when code is ready, cancel superseded runs. Watch with `gh
run watch <id> -R gesher-runtime/gesher --exit-status --interval 120`; on
HTTP 403 back off ~10 minutes. Keep a "Status and next steps" section
updated with every meaningful push. Before your final CI run, merge
origin/phase4/integration into your branch. Final message = the report
track-rules.md asks for (run ids, PASS lines, remaining gaps, edits outside
your domain, the verified SHA).
