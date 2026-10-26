# Track brief: surfaces2

Reserved ADR: **0063**. Continue the remaining graphics/GL scope after the
verified first surfaces stage is merged. Create wip/surfaces2 from current
phase4/integration in its own worktree; no implementation is started yet.

Read track-rules.md, graphics.md's surfaces status, ADRs 0006/0052/0056,
the SurfaceTexture/TextureLayer and child EGL bridge, the native3 bitmap
bridge and the complete roadmap/ADR graphics limits before implementation.

## Required work

- Java EGL14/GLES20 natives and GLSurfaceView, including a renderer lifecycle
  on the GL thread, context loss/recreation, resize/pause/resume and camera
  preview into that path. Reuse pinned AOSP implementation where practical.
- SurfaceTexture consumed by GLES: attach/detach, updateTexImage, transform
  matrix/timestamps and external-OES texture sampling. CPU TextureView
  rendering alone does not establish this path.
- Child SurfaceControl transactions: alpha/transforms and the remaining
  layering/clipping/ownership limits in the surfaces ADR/status section.
- YuvImage JPEG_R and the remaining Bitmap parcel storage/ownership limits
  in ADR 0056; the first stage's heap-copy reader is not evidence of mapped
  Bitmap storage semantics.
- Activity UI rendered with SkiaGL: correct composed pixels through
  PrintWindow, including the previously white readback gap and popups where
  their SkiaGL opacity/composition remains open.
- NDK native-window/hardware-buffer and choreographer entries assigned to
  surfaces2 in native3's docs/handoff/native-api-coverage.json. Symbol
  presence alone is insufficient: test real storage, usage/format/stride,
  ownership, fences and callbacks. Coordinate Bitmap hardware storage with
  native3 and media buffers with media3.
- Remaining ADR 0030 GL paths: hardware Bitmap/HardwareBufferRenderer,
  WebView functors, persistent shader cache and GPU memory budgeting.
  Vulkan remains a separate native3 implementation contract. Keep real-GPU
  verification explicitly open where the runners only provide WARP.

## Verification and domain

Use separate tests/windows/tracks/53-surfaces2.ps1 and small Java/NDK apps,
both ABIs where native calls apply, single process and device. Require
pixel readback, lifecycle/callback ordering, error paths and resource cleanup;
keep 52-surfaces, native2/native3 bitmap checks, media and TIC-80 green.
Run full CI on both runners with package and both e2e jobs before merge.
Never build AOSP locally. Tick only the specific CI-verified roadmap rows.

Own compat/media/surface, compat/hwui/surfaces and the required window/GL
bridges, frameworks/base patches for these features, new tests and docs.
Shared CMake/CI/Services/installer edits must be minimal and retain the other
tracks' intent. Never drop a feature merely because the CI runner lacks
hardware; distinguish a tested software contract from real hardware evidence.

## Status and next steps

Unstarted. The first surfaces stage merged as 4f99afd after full ci
40307919084 attempt 2; its retained open items are assigned here.
This reservation does not close them or claim hardware support.
