# ADR 0063: Retry failed SurfaceView invalidations before coalescing later frames

Status: Proposed (source only; CI pending)

## Context

A SurfaceView's QueueSurface calls a native listener for each published
frame. The listener coalesces requests until the view records a draw. Its
pending flag previously stayed set if no JNI environment or live weak view
was available, or if Java postInvalidate threw. Drawing also cleared pending
before publishing the weak view. A producer could therefore leave pending
set without a queued Java invalidation, preventing subsequent frames from
requesting a redraw.

TIC-80 rendered a blank initial PrintWindow capture in the 2025 e2e job of
full40487303965 attempt2, while its later click changed all320000 pixels.
Both recorded app logs complete EGL initialization and show a loaded
installed font map after fonts.xml ENOENT. These observations do not establish
which condition caused that original blank frame.

## Decision

Use a small FrameInvalidation gate in the production listener. A failed
post releases its own generation's pending state, so the next producer frame
can retry. A delayed failure cannot clear a newer successful request after
an intervening draw. A successful post
coalesces later requests until drawing publishes the weak view and then
acknowledges the invalidation. Preserve the actual JNI post, exception and
reference handling. Bounded opt-in traces expose request outcome, latest
frame, draw and consumed frame; unsampled pixel data is labelled explicitly.

Compile and run seven controlled gate tests only in CI, including the
old-failure/draw/new-post ordering under a barrier. Bound process, post-kill
and pipe-drain waits and fail on unfinished pipes. Exercise real EGL producer
color changes without input or explicit View.invalidate on both native ABIs,
in single-process and fresh-device modes. Retain the original SurfaceGL and
TIC-80 checks. After an original blank-frame failure, collect fifteen seconds
of no-input timing/hash/pixel evidence before the original click, without
changing the failure result or thresholds.

## Limits and verification

No build or runtime acceptance is claimed yet. A full green Windows matrix,
package and both ten-app e2e jobs are required before accepting this slice.
The controlled gate and real phase change establish bounded liveness
contracts; they do not retroactively prove the cause of an earlier e2e
failure. Java EGL/GLES/GLSurfaceView, SurfaceTexture external-OES sampling,
hardware buffers, Activity SkiaGL readback and all other Surface2 limits
remain open in its track brief.
