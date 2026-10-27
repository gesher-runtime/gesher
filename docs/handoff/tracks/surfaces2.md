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

The in-progress correction is in C:\work\gesher-media4-invalidation on
wip/media4-invalidation, based on the accepted Windows integration6297e80a
and Media4's separate format/OpenSL/native/AAudio candidate. No compiler or
native/JVM/C# fixture is run locally. Root completed bounded source review
of exact d239a7e38658ab2853a1344ccf5f893cba28c8bf after three review fixes.
Full ci40507882249 is complete RED at
06d30d403481d3833f0c7d5f99db456841b72062, with20 successful jobs, six skipped
and two failed Windows suites. Root reviewed exact d239; only integration
10309b0a's services3.md documentation was added before that full. Source06
remains immutable evidence, with no unchanged rerun or inventory acceptance.
Both completed Windows logs and all five critical test/package/e2e logs
were read. Windows2022 ends4 test(s) failed and2025 ends2, exclusively new
test80. All seven production-gate controls and prior Media4 format10,
OpenSL16 and AAudio16 PASS lines pass on each host. Real x86 phase captures
pass on2025 in both modes. On2022 the phase1 marker is visibly interleaved
with native stderr traces, so the helper never takes either phase capture;
this is not a failed pixel comparison. Both ARM modes crash on both hosts
before the native ABI marker, with approximate guest pc2544 in
libsurfacegl.so and startPhases+92/pc2400. Neither the missing marker nor the
guest-read label alone proves which ordinary call failed.

Package122223068647 passes all23 actual installer/window/input/uninstall
checks with the exact installed OpenSL notice SHA256
B78E6FE1973849F5F31CBF37835C907174D6A816721FD89E5EB734921F54E8DE.
Both actual ten-app e2e jobs122224349335/518 pass with0 unexpected failures,
including original mandatory untraced TIC and the separate mandatory traced
scenario, four transport controls and association/shortcut launches. Default
TIC shows8713/8729 non-background and2441 text pixels on2022/2025. This does
not retroactively prove the cause of the older blank observation.

A CI-only static symbol inspection of the exact recorded surfaces artifact
is complete; no artifact is downloaded locally, no app is rerun and no local
compiler/fixture executes. Native owns generic ARM va_list conversion in
its separate worktree. Its bounded hypothesis is an eager stack copy during
ordinary variadic formatting; exact call-site/bounds evidence is required.
The original logger cold path must remain covered even if the independent
one-write logging correction changes the phase marker transport. Runtime
repair and the final coherent full run remain open.

Static symbol ci40523288209 is GREEN at063c7326, reading only the exact
recorded06 surfaces artifact inside CI. The actual disassembly proves
startPhases0x23fc calls say and returns at0x2400; say0x2540 calls vprintf@plt
and returns at0x2544, matching the crash backtrace. This narrows the ordinary
call site without claiming runtime stack bounds. Native owns the generic
conversion fix and its new controlled evidence.

The independent logger correction now composes each marker and sends one
write(1) instead of three stdio writes sharing the log with stderr traces.
It preserves the original printf/vprintf/newline/fflush body as a noinline
legacy_say cold regression. Before any phase marker, the real fixture calls
that path with a zero-argument literal and eight integer arguments; test80
requires an atomic return marker in addition to the unchanged ABI, phase,
color, teardown and shutdown assertions. The formatter itself still needs
the generic ARM va_list correction. No passing runtime result or old-cause
claim follows from this new source preparation.

Compile-only ci40526285481 is GREEN at
fd32798f3e81b5366ecc4e15effb48c9cea99947. The actual surfaces job122266656262
log and metadata were read: both x86_64 and arm64-v8a native libraries and
their separate APKs build successfully. The new compile-only workflow
reuses surfaces.yml; it executes no Android app. This proves source/build
compatibility only. Native's separate runtime bounds/ABI correction and a
new strict coherent full run are still required.

The correction branch also merges accepted integration13603d54 before its
next runtime candidate, retaining the accepted Net4 implementation and its
verification metadata. The only merge conflict was the handoff reservation
paragraph: all current assignments and the unaccepted AAudio status are
retained. All Surface production, fixture, phase/original-TIC and build
sources stay unchanged by this prerequisite merge. Native's separately
reviewed ABI correction is not imported yet.

Native's controlled baseline ci40525678026 now supplies actual bounds for
the unchanged original Surface logger body, independently of this app.
Both Windows job logs were read: deep calls return0 and print the literal;
shallow calls return139 with7 GP/8 FP slots and a496-byte eager copy ending
exactly0xa0 past the guest stack top. This proves an invalid speculative
read for that valid controlled call. Static inspection places the original
Surface fault at the same vprintf return seam, but this does not replace a
corrected Surface run or prove the older TIC blank cause. Native owns the
tagged-list correction, API-version rejection and its component/full proof.

The coordinator now requires a focused coherent component before another
broad full: import Native's exact API3/Locale2/FPSR prerequisite, build fresh
dalvikvm/gesher-exec/DLL and use source-matched jars/images, then run the
unchanged test80 and86 on both runners. The prepared component driver loads
the original SurfaceRun function/type from the track52 AST and the original
WindowChecks module, retaining every pixel/phase/ABI/teardown gate without
copying their implementation. Runtime/boot/font setup follows the full
harness. The workflow source guard and Native prerequisite are not ready
yet; no component dispatch or original Surface repair is claimed. Broad
full also waits for the separate common Web3 correction. Muxer full405277
remains an independent frozen candidate.

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

Separate test-infrastructure leftovers were reported to the coordinator:
legacy SurfaceRun in52-surfaces.ps1 and InvokeOpenSl in76-media4-opensl.ps1
still use an unbounded post-kill WaitForExit(). The new helper in test80
bounds termination and both redirected pipes, but the legacy shared helpers
need their own reviewed correction and CI evidence outside this frozen
candidate. This source review does not mark those leftovers fixed.
