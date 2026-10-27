# Track brief: surfaces2

Reserved ADR: **0063**. Continue the remaining graphics/GL scope after the
verified first surfaces stage is merged. Create wip/surfaces2 from current
phase4/integration in its own worktree. The first bounded invalidation
correction is being prepared alongside the coherent Media4 candidate.

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

Use separate tests/windows/tracks/80-surface-invalidation.ps1 for the first
invalidation slice and small Java/NDK apps,
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

The first surfaces stage merged as 4f99afd after full ci40307919084
attempt2; all its retained graphics limits remain assigned here. No broad
Surface2 roadmap row is closed by the first bounded correction below.

Source-only work is in C:\work\gesher-media4-invalidation on
wip/media4-invalidation, based on the accepted Windows integration6297e80a
and Media4's separate format/OpenSL/native/AAudio candidate. No compiler or
native/JVM/C# fixture is run locally. Full CI is pending source review.

ViewSurfaceFor previously latched pending before obtaining an environment
or live weak view and left it latched after a failed Java post. Draw also
cleared pending before publishing its weak view. The correction retries
after failed posts and binds the weak view before acknowledging the draw.
A small production gate is shared with seven CI-only controlled tests:
no-env/no-view/Java-post failure followed by recovery, pending coalescing,
a barrier that holds binding before acknowledgement, concurrent producers,
and a delayed old failure after a newer successful post. Generation-tagged
pending requests prevent that old failure from clearing the newer request.
Root identified the missing generation guard during source review; no
runtime acceptance follows from that review. All new process, post-kill
and redirected-pipe waits are bounded, with unfinished pipes a failure.
Opt-in bounded traces record actual latest-frame/request outcome,
draw and consumed-frame numbers; a bounded pixel sample distinguishes black
producer frames without making an unsampled value a pixel-count claim.

The real SurfaceView/EGL app gains a separate activity: only the producer
changes red/green to blue/yellow after twelve seconds. Test80 requires both
captures with the existing color thresholds, actual native ABI markers,
complete teardown and bounded device shutdown, in single-process and fresh
device modes on both x86_64 and arm64-v8a. No input, resize or explicit
View.invalidate is sent to this activity. The original SurfaceGL activity
and its checks remain enabled.

This source defect has not been proved to cause TIC-80's observed blank
frame in Media4 full40487303965 attempt2. The original blank/text assertions
and thresholds are unchanged. Only after an original blank assertion fails,
the harness records fifteen one-second no-input captures with elapsed/UTC
time, hash and pixel counts before its original click. The original mandatory
TIC run explicitly unsets both surface/window trace flags. Its result and
app/launcher logs are recorded first, then a separate mandatory fresh-device
scenario enables bounded surface/window traces. This avoids accepting the
product default only under instrumentation that changes startup timing.
Neither a traced pass nor a later nonblank diagnostic capture can turn the
original failure into a pass. Both recorded app logs contain
fonts.xml ENOENT followed by successful installed font-map loading; neither
the first reported exception nor the source defect is a proven cause yet.

Java EGL14/GLES20/GLSurfaceView, SurfaceTexture external-OES consumption,
child transactions, Activity SkiaGL readback, hardware storage/fences,
Vulkan and every other required item above remain open. No new API coverage
metadata or roadmap acceptance is claimed before a complete green full run.
